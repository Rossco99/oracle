#include "drops.hpp"
#include "ram.hpp"
#include <eosio.system/exchange_state.hpp>

checksum256 drops::compute_epoch_value(uint64_t epoch)
{
   // Load the epoch and ensure all secrets have been revealed
   drops::epochs_table epochs(_self, _self.value);
   auto                epoch_itr = epochs.find(epoch);
   check(epoch_itr != epochs.end(), "Epoch does not exist");
   // TODO: Check a value to ensure the epoch has been completely revealed

   // Load all reveal values for the epoch
   drops::reveals_table reveals_table(_self, _self.value);
   auto                 reveal_idx = reveals_table.get_index<"epoch"_n>();
   auto                 reveal_itr = reveal_idx.find(epoch);
   check(reveal_itr != reveal_idx.end(), "Epoch has no reveal values?");

   // Accumulator for all reveal values
   std::vector<std::string> reveals;

   // Iterate over reveals and build a vector containing them all
   while (reveal_itr != reveal_idx.end() && reveal_itr->epoch == epoch) {
      reveals.push_back(reveal_itr->reveal);
      reveal_itr++;
   }

   // Sort the reveal values alphebetically for consistency
   sort(reveals.begin(), reveals.end());

   // Combine the epoch, drops, and reveals into a single string
   string result = std::to_string(epoch);
   for (const auto& reveal : reveals)
      result += reveal;

   // Generate the sha256 value of the combined string
   return sha256(result.c_str(), result.length());
}

checksum256 drops::compute_epoch_drops_value(uint64_t epoch, uint64_t drops)
{
   // Load the drops
   drops::drop_table drops(_self, _self.value);
   auto              drops_itr = drops.find(drops);
   check(drops_itr != drops.end(), "Drop not found");

   // Ensure this drops was valid for the given epoch
   // A drops must be created before or during the provided epoch
   check(drops_itr->epoch <= epoch, "Drop was generated after this epoch and is not valid for computation.");

   // Load the epoch drops value
   drops::epochdrop_table epochdrops(_self, _self.value);
   auto                   epochdrops_itr = epochdrops.find(epoch);
   check(epochdrops_itr != epochdrops.end(), "Epoch has not yet been resolved.");

   // Generate the sha256 value of the combined string
   return drops::hash(epochdrops_itr->drops, drops);
}

checksum256 drops::compute_last_epoch_drops_value(uint64_t drops)
{
   // Load current state
   drops::state_table state(_self, _self.value);
   auto               state_itr = state.find(1);
   uint64_t           epoch     = state_itr->epoch;
   // Set to previous epoch
   uint64_t last_epoch = epoch - 1;
   // Return value for the last epoch
   return compute_epoch_drops_value(last_epoch, drops);
}

[[eosio::action]] checksum256 drops::computedrops(uint64_t epoch, uint64_t drops)
{
   return compute_epoch_drops_value(epoch, drops);
}

[[eosio::action]] checksum256 drops::computeepoch(uint64_t epoch) { return compute_epoch_value(epoch); }

[[eosio::action]] checksum256 drops::cmplastepoch(uint64_t drops, name contract)
{
   require_recipient(contract);
   return compute_last_epoch_drops_value(drops);
}

drops::epoch_row drops::advance_epoch()
{
   // Retrieve contract state
   drops::state_table state(_self, _self.value);
   auto               state_itr = state.find(1);
   uint64_t           epoch     = state_itr->epoch;
   check(state_itr->enabled, "Contract is currently disabled.");

   // Retrieve current epoch based on state
   drops::epochs_table epochs(_self, _self.value);
   auto                epoch_itr = epochs.find(epoch);
   check(epoch_itr != epochs.end(), "Epoch from state does not exist.");
   check(current_time_point() >= epoch_itr->end,
         "Current epoch " + std::to_string(epoch) + " has not ended (" + epoch_itr->end.to_string() + ").");

   // Advance epoch number
   uint64_t new_epoch = epoch + 1;

   // Update the epoch in state
   state.modify(state_itr, _self, [&](auto& row) { row.epoch = new_epoch; });

   // Base the next epoch off the current epoch
   time_point new_epoch_start    = epoch_itr->end;
   time_point new_epoch_end      = epoch_itr->end + eosio::seconds(epochphasetimer);
   time_point new_epoch_complete = epoch_itr->end + eosio::seconds(epochphasetimer * 10);

   std::vector<name>    oracles;
   drops::oracles_table oracles_table(_self, _self.value);
   auto                 oracle_itr = oracles_table.begin();
   check(oracle_itr != oracles_table.end(), "No oracles registered, cannot init.");
   while (oracle_itr != oracles_table.end()) {
      oracles.push_back(oracle_itr->oracle);
      oracle_itr++;
   }

   // Save the next epoch
   epochs.emplace(_self, [&](auto& row) {
      row.epoch     = new_epoch;
      row.start     = new_epoch_start;
      row.end       = new_epoch_end;
      row.oracles   = oracles;
      row.completed = 0;
   });

   // Nofify subscribers
   drops::subscribers_table subscribers(_self, _self.value);
   auto                     subscriber_itr = subscribers.begin();
   while (subscriber_itr != subscribers.end()) {
      require_recipient(subscriber_itr->subscriber);
      subscriber_itr++;
   }

   // Return the next epoch
   return {
      new_epoch,       // epoch
      new_epoch_start, // start
      new_epoch_end,   // end
      oracles,         // oracles
      0                // completed
   };
}

[[eosio::on_notify("eosio.token::transfer")]] drops::generate_return_value
drops::generate(name from, name to, asset quantity, std::string memo)
{
   if (from == "eosio.ram"_n || to != _self || from == _self || memo == "bypass") {
      return {(uint32_t)0, (uint64_t)0, asset{0, EOS}, asset{0, EOS}, (uint64_t)0, (uint64_t)0};
   }

   require_auth(from);
   check(quantity.amount > 0, "The transaction amount must be a positive value.");
   check(quantity.symbol == EOS, "Only the system token is accepted for transfers.");
   check(!memo.empty(), "A memo is required to send tokens to this contract");

   // Retrieve contract state
   state_table state(_self, _self.value);
   auto        state_itr = state.find(1);
   uint64_t    epoch     = state_itr->epoch;
   check(state_itr->enabled, "Contract is currently disabled.");

   epochs_table epochs(_self, _self.value);
   auto         epoch_itr = epochs.find(epoch);
   check(epoch_itr != epochs.end(), "Epoch does not exist.");

   time_point epoch_end = epoch_itr->end;

   // Process the memo field to determine the number of drops to generate
   std::vector<std::string> parsed = split(memo, ',');
   check(parsed.size() == 2, "Memo data must contain 2 values, seperated by a comma: amount,drops_data.");

   // Ensure amount is a positive value
   int amount = stoi(parsed[0]);
   check(amount > 0, "The amount of drops to generate must be a positive value.");

   // Ensure string length
   string data = parsed[1];
   check(data.length() > 32, "Drop data must be at least 32 characters in length.");

   // Calculate amount of RAM needing to be purchased
   // TODO: Additional RAM is being purchased to account for the buyrambytes bug
   // https://github.com/EOSIO/eosio.system/issues/30
   uint64_t ram_purchase_amount = amount * (record_size + purchase_buffer);

   // Determine if this account exists in the accounts table
   accounts_table accounts(_self, _self.value);
   auto           account_itr        = accounts.find(from.value);
   bool           account_row_exists = account_itr != accounts.end();

   // First time accounts must purchase the extra ram to persist their stats in memory
   // TODO: Need to decide if we should persist this data or not
   if (!account_row_exists) {
      ram_purchase_amount += accounts_row;
   }

   // Determine if this account exists in the epoch stats table
   stats_table stats(_self, _self.value);
   auto        stat_idx        = stats.get_index<"accountepoch"_n>();
   auto        stat_itr        = stat_idx.find((uint128_t)from.value << 64 | epoch);
   bool        stat_row_exists = stat_itr != stat_idx.end();

   //    drops::commits_table commits(_self, _self.value);
   //    auto                commit_idx = commits.get_index<"oracle"_n>();
   //    auto                commit_itr = commit_idx.find(oracle.value);
   //    check(commit_itr == commit_idx.end(), "Oracle has already committed");

   // First time epoch stats must purchase the extra ram to persist their stats in memory
   // TODO: Need to decide if we should persist this data or not
   if (!stat_row_exists) {
      ram_purchase_amount += stats_row;
   }

   // Purchase the RAM
   action(permission_level{_self, "active"_n}, "eosio"_n, "buyrambytes"_n,
          std::make_tuple(_self, _self, ram_purchase_amount))
      .send();

   // Iterate over all drops to be created and insert them into the drops table
   drop_table drops(_self, _self.value);
   for (int i = 0; i < amount; i++) {
      string   value      = std::to_string(i) + data;
      auto     hash       = sha256(value.c_str(), value.length());
      auto     byte_array = hash.extract_as_byte_array();
      uint64_t drops;
      memcpy(&drops, &byte_array, sizeof(uint64_t));
      drops.emplace(_self, [&](auto& row) {
         row.drops = drops;
         row.owner = from;
         row.epoch = epoch;
      });
   }

   // Either update the account row or insert a new row
   uint64_t new_drops_total = amount;
   if (account_row_exists) {
      new_drops_total += account_itr->drops;
      accounts.modify(account_itr, _self, [&](auto& row) { row.drops = new_drops_total; });
   } else {
      accounts.emplace(_self, [&](auto& row) {
         row.account = from;
         row.drops   = new_drops_total;
      });
   }

   // Either update the stats row or insert a new row
   uint64_t new_drops_epoch = amount;
   if (stat_row_exists) {
      new_drops_epoch += stat_itr->drops;
      stat_idx.modify(stat_itr, _self, [&](auto& row) { row.drops = new_drops_epoch; });
   } else {
      stats.emplace(_self, [&](auto& row) {
         row.id      = stats.available_primary_key();
         row.account = from;
         row.drops   = new_drops_epoch;
         row.epoch   = epoch;
      });
   }

   // Calculate the purchase cost via bancor after the purchase to ensure the incoming transfer can cover it
   asset ram_purchase_cost = eosiosystem::ramcostwithfee(ram_purchase_amount, EOS);
   check(quantity.amount >= ram_purchase_cost.amount,
         "The amount sent does not cover the RAM purchase cost (requires " + ram_purchase_cost.to_string() + ")");

   // Calculate any remaining tokens from the transfer after the RAM purchase
   int64_t remainder = quantity.amount - ram_purchase_cost.amount;

   // Return any remaining tokens to the sender
   if (remainder > 0) {
      action{permission_level{_self, "active"_n}, "eosio.token"_n, "transfer"_n,
             std::tuple<name, name, asset, std::string>{_self, from, asset{remainder, EOS}, ""}}
         .send();
   }

   return {
      (uint32_t)amount,      // drops bought
      epoch,                 // epoch
      ram_purchase_cost,     // cost
      asset{remainder, EOS}, // refund
      new_drops_total,       // total drops
      new_drops_epoch,       // epoch drops
   };
}

[[eosio::action]] drops::generate_return_value drops::generatertrn() {}

[[eosio::action]] void drops::transfer(name from, name to, std::vector<uint64_t> drops_ids, string memo)
{
   require_auth(from);
   check(is_account(to), "Account does not exist.");
   require_recipient(from);
   require_recipient(to);

   // Retrieve contract state
   state_table state(_self, _self.value);
   auto        state_itr = state.find(1);
   check(state_itr->enabled, "Contract is currently disabled.");

   check(drops_ids.size() > 0, "No drops were provided to transfer.");

   drops::drop_table drops(_self, _self.value);

   // Map to record which epochs drops were destroyed in
   map<uint64_t, uint64_t> epochs_transferred_in;

   for (auto it = begin(drops_ids); it != end(drops_ids); ++it) {
      auto drops_itr = drops.find(*it);
      check(drops_itr != drops.end(), "Drop not found");
      check(drops_itr->owner == from, "Account does not own this drops");
      // Incremenent the values of all epochs we destroyed drops in
      if (epochs_transferred_in.find(drops_itr->epoch) == epochs_transferred_in.end()) {
         epochs_transferred_in[drops_itr->epoch] = 1;
      } else {
         epochs_transferred_in[drops_itr->epoch] += 1;
      }
      // Perform the transfer
      drops.modify(drops_itr, _self, [&](auto& row) { row.owner = to; });
   }

   accounts_table accounts(_self, _self.value);
   auto           account_from_itr = accounts.find(from.value);
   check(account_from_itr != accounts.end(), "From account not found");
   accounts.modify(account_from_itr, _self, [&](auto& row) { row.drops = row.drops - drops_ids.size(); });

   auto account_to_itr = accounts.find(to.value);
   if (account_to_itr != accounts.end()) {
      accounts.modify(account_to_itr, _self, [&](auto& row) { row.drops = row.drops + drops_ids.size(); });
   } else {
      accounts.emplace(from, [&](auto& row) {
         row.account = to;
         row.drops   = drops_ids.size();
      });
   }

   stats_table stats(_self, _self.value);

   // Iterate over map that recorded which epochs were transferred in for from, decrement table values
   for (auto& iter : epochs_transferred_in) {
      auto stat_idx = stats.get_index<"accountepoch"_n>();
      auto stat_itr = stat_idx.find((uint128_t)from.value << 64 | iter.first);
      stat_idx.modify(stat_itr, _self, [&](auto& row) { row.drops = row.drops - iter.second; });
   }

   // Iterate over map that recorded which epochs were transferred in for to, increment table values
   for (auto& iter : epochs_transferred_in) {
      auto stat_idx        = stats.get_index<"accountepoch"_n>();
      auto stat_itr        = stat_idx.find((uint128_t)to.value << 64 | iter.first);
      bool stat_row_exists = stat_itr != stat_idx.end();
      if (stat_row_exists) {
         stat_idx.modify(stat_itr, _self, [&](auto& row) { row.drops = row.drops + iter.second; });
      } else {
         stats.emplace(from, [&](auto& row) {
            row.id      = stats.available_primary_key();
            row.account = to;
            row.drops   = iter.second;
            row.epoch   = iter.first;
         });
      }
   }
}

[[eosio::action]] drops::destroy_return_value drops::destroy(name owner, std::vector<uint64_t> drops_ids, string memo)
{
   require_auth(owner);

   // Retrieve contract state
   state_table state(_self, _self.value);
   auto        state_itr = state.find(1);
   check(state_itr->enabled, "Contract is currently disabled.");

   check(drops_ids.size() > 0, "No drops were provided to destroy.");
   //    check(drops_ids.size() <= 5000, "Cannot destroy more than 5000 at a time.");

   drops::drop_table drops(_self, _self.value);

   // Map to record which epochs drops were destroyed in
   map<uint64_t, uint64_t> epochs_destroyed_in;

   // Loop to destroy specified drops
   for (auto it = begin(drops_ids); it != end(drops_ids); ++it) {
      auto drops_itr = drops.find(*it);
      check(drops_itr != drops.end(), "Drop not found");
      check(drops_itr->owner == owner, "Account does not own this drops");
      // Incremenent the values of all epochs we destroyed drops in
      if (epochs_destroyed_in.find(drops_itr->epoch) == epochs_destroyed_in.end()) {
         epochs_destroyed_in[drops_itr->epoch] = 1;
      } else {
         epochs_destroyed_in[drops_itr->epoch] += 1;
      }
      // Destroy the drops
      drops.erase(drops_itr);
   }

   // Iterate over map that recorded which epochs were destroyed in, decrement table values
   for (auto& iter : epochs_destroyed_in) {
      stats_table stats(_self, _self.value);
      auto        stat_idx = stats.get_index<"accountepoch"_n>();
      auto        stat_itr = stat_idx.find((uint128_t)owner.value << 64 | iter.first);
      stat_idx.modify(stat_itr, _self, [&](auto& row) { row.drops = row.drops - iter.second; });
   }

   // Decrement the account row
   accounts_table accounts(_self, _self.value);
   auto           account_itr = accounts.find(owner.value);
   accounts.modify(account_itr, _self, [&](auto& row) { row.drops = row.drops - drops_ids.size(); });

   // Calculate RAM sell amount and proceeds
   uint64_t ram_sell_amount   = drops_ids.size() * record_size;
   asset    ram_sell_proceeds = eosiosystem::ramproceedstminusfee(ram_sell_amount, EOS);

   action(permission_level{_self, "active"_n}, "eosio"_n, "sellram"_n, std::make_tuple(_self, ram_sell_amount)).send();

   token::transfer_action transfer_act{"eosio.token"_n, {{_self, "active"_n}}};
   //    check(false, "ram_sell_proceeds: " + ram_sell_proceeds.to_string());
   transfer_act.send(_self, owner, ram_sell_proceeds,
                     "Reclaimed RAM value of " + std::to_string(drops_ids.size()) + " drops(s)");

   return {
      ram_sell_amount,  // ram sold
      ram_sell_proceeds // redeemed ram value
   };
}

[[eosio::action]] void drops::destroyall()
{
   require_auth(_self);

   uint64_t            drops_destroyed = 0;
   map<name, uint64_t> drops_destroyed_for;

   drops::drop_table drops(_self, _self.value);
   auto              drops_itr = drops.begin();
   while (drops_itr != drops.end()) {
      drops_destroyed += 1;
      // Keep track of how many drops were destroyed per owner for debug refund
      if (drops_destroyed_for.find(drops_itr->owner) == drops_destroyed_for.end()) {
         drops_destroyed_for[drops_itr->owner] = 1;
      } else {
         drops_destroyed_for[drops_itr->owner] += 1;
      }
      drops_itr = drops.erase(drops_itr);
   }

   drops::accounts_table accounts(_self, _self.value);
   auto                  account_itr = accounts.begin();
   while (account_itr != accounts.end()) {
      account_itr = accounts.erase(account_itr);
   }

   drops::stats_table stats(_self, _self.value);
   auto               stats_itr = stats.begin();
   while (stats_itr != stats.end()) {
      stats_itr = stats.erase(stats_itr);
   }

   // Calculate RAM sell amount
   uint64_t ram_to_sell = drops_destroyed * record_size;
   action(permission_level{_self, "active"_n}, "eosio"_n, "sellram"_n, std::make_tuple(_self, ram_to_sell)).send();

   for (auto& iter : drops_destroyed_for) {
      uint64_t ram_sell_amount   = iter.second * record_size;
      asset    ram_sell_proceeds = eosiosystem::ramproceedstminusfee(ram_sell_amount, EOS);

      token::transfer_action transfer_act{"eosio.token"_n, {{_self, "active"_n}}};
      //    check(false, "ram_sell_proceeds: " + ram_sell_proceeds.to_string());
      transfer_act.send(_self, iter.first, ram_sell_proceeds,
                        "Testnet Reset - Reclaimed RAM value of " + std::to_string(iter.second) + " drops(s)");
   }
}

[[eosio::action]] void drops::enroll(name account, uint64_t epoch)
{
   require_auth(account);

   // Determine if this account exists in the accounts table
   accounts_table accounts(_self, _self.value);
   auto           account_itr        = accounts.find(account.value);
   bool           account_row_exists = account_itr != accounts.end();

   // Register the account into the accounts table
   if (!account_row_exists) {
      accounts.emplace(account, [&](auto& row) {
         row.account = account;
         row.drops   = 0;
      });
   }

   // Determine if this account exists in the epoch stats table for the epoch
   stats_table stats(_self, _self.value);
   auto        stat_idx = stats.get_index<"accountepoch"_n>();
   auto        stat_itr = stat_idx.find((uint128_t)account.value << 64 | epoch);
   check(stat_itr != stat_idx.end(), "This account is already registered for this epoch.");

   stats.emplace(account, [&](auto& row) {
      row.id      = stats.available_primary_key();
      row.account = account;
      row.drops   = 0;
      row.epoch   = epoch;
   });
}

[[eosio::action]] drops::epoch_row drops::advance()
{
   // Advance the epoch
   auto new_epoch = advance_epoch();

   // Advance until we exist in the current epoch
   while (current_time_point() >= new_epoch.end) {
      new_epoch = advance_epoch();
   }

   // Provide the epoch as a return value
   return new_epoch;
}

[[eosio::action]] void drops::commit(name oracle, uint64_t epoch, checksum256 commit)
{
   require_auth(oracle);

   // Retrieve contract state
   state_table state(_self, _self.value);
   auto        state_itr = state.find(1);
   check(state_itr->enabled, "Contract is currently disabled.");

   drops::epochs_table epochs(_self, _self.value);
   auto                epoch_itr = epochs.find(epoch);
   check(epoch_itr != epochs.end(), "Epoch does not exist");
   check(std::find(epoch_itr->oracles.begin(), epoch_itr->oracles.end(), oracle) != epoch_itr->oracles.end(),
         "Oracle is not in the list of oracles for this epoch");

   auto current_time = current_time_point();
   check(current_time > epoch_itr->start, "Epoch not started");
   check(current_time < epoch_itr->end, "Epoch no longer accepting commits");

   drops::commits_table commits(_self, _self.value);
   auto                 commit_idx = commits.get_index<"epochoracle"_n>();
   auto                 commit_itr = commit_idx.find(((uint128_t)oracle.value << 64) + epoch);
   check(commit_itr == commit_idx.end(), "Oracle has already committed");

   commits.emplace(_self, [&](auto& row) {
      row.id     = commits.available_primary_key();
      row.epoch  = epoch;
      row.oracle = oracle;
      row.commit = commit;
   });
}

[[eosio::action]] void drops::reveal(name oracle, uint64_t epoch, string reveal)
{
   require_auth(oracle);

   // Retrieve contract state
   state_table state(_self, _self.value);
   auto        state_itr = state.find(1);
   check(state_itr->enabled, "Contract is currently disabled.");

   drops::epochs_table epochs(_self, _self.value);
   auto                epoch_itr = epochs.find(epoch);
   check(epoch_itr != epochs.end(), "Epoch does not exist");
   check(epoch_itr->completed == 0, "Epoch has already completed");

   auto current_time = current_time_point();
   check(current_time > epoch_itr->end, "Epoch has not concluded");

   drops::reveals_table reveals(_self, _self.value);
   auto                 reveal_idx = reveals.get_index<"epochoracle"_n>();
   auto                 reveal_itr = reveal_idx.find(((uint128_t)oracle.value << 64) + epoch);
   check(reveal_itr == reveal_idx.end(), "Oracle has already revealed");

   drops::commits_table commits(_self, _self.value);
   auto                 commit_idx = commits.get_index<"epochoracle"_n>();
   auto                 commit_itr = commit_idx.find(((uint128_t)oracle.value << 64) + epoch);
   check(commit_itr != commit_idx.end(), "Oracle never committed");

   checksum256 reveal_hash = sha256(reveal.c_str(), reveal.length());
   auto        reveal_arr  = reveal_hash.extract_as_byte_array();

   checksum256 commit_hash = commit_itr->commit;
   auto        commit_arr  = commit_hash.extract_as_byte_array();

   check(reveal_hash == commit_hash,
         "Reveal value '" + reveal + "' hashes to '" + hexStr(reveal_arr.data(), reveal_arr.size()) +
            "' which does not match commit value '" + hexStr(commit_arr.data(), commit_arr.size()) + "'.");

   reveals.emplace(_self, [&](auto& row) {
      row.id     = reveals.available_primary_key();
      row.epoch  = epoch;
      row.oracle = oracle;
      row.reveal = reveal;
   });

   // TODO: This logic is the exact same as finishreveal, should be refactored
   vector<name> has_revealed;
   auto         completed_reveals_idx = reveals.get_index<"epochoracle"_n>();
   for (name oracle : epoch_itr->oracles) {
      auto completed_reveals_itr = completed_reveals_idx.find(((uint128_t)oracle.value << 64) + epoch);
      if (completed_reveals_itr != completed_reveals_idx.end()) {
         has_revealed.push_back(oracle);
      }
   }
   if (has_revealed.size() == epoch_itr->oracles.size()) {
      // Complete the epoch
      epochs.modify(epoch_itr, _self, [&](auto& row) { row.completed = 1; });
      // Persist the epoch drops
      drops::epochdrop_table epochdrops(_self, _self.value);
      epochdrops.emplace(_self, [&](auto& row) {
         row.epoch = epoch;
         row.drops = compute_epoch_value(epoch);
      });
   }

   // TODO: Create an administrative action that can force an Epoch completed if an oracle fails to reveal.
}

[[eosio::action]] void drops::finishreveal(uint64_t epoch)
{
   drops::epochs_table epochs(_self, _self.value);
   auto                epoch_itr = epochs.find(epoch);
   check(epoch_itr != epochs.end(), "Epoch does not exist");
   check(epoch_itr->completed == 0, "Epoch has already completed");

   vector<name>         has_revealed;
   drops::reveals_table reveals(_self, _self.value);
   auto                 reveals_idx = reveals.get_index<"epochoracle"_n>();
   for (name oracle : epoch_itr->oracles) {
      auto completed_reveals_itr = reveals_idx.find(((uint128_t)oracle.value << 64) + epoch);
      if (completed_reveals_itr != reveals_idx.end()) {
         has_revealed.push_back(oracle);
      }
   }

   if (has_revealed.size() == epoch_itr->oracles.size()) {
      // Complete the epoch
      epochs.modify(epoch_itr, _self, [&](auto& row) { row.completed = 1; });
      // Persist the computed epoch drops
      drops::epochdrop_table epochdrops(_self, _self.value);
      epochdrops.emplace(_self, [&](auto& row) {
         row.epoch = epoch;
         row.drops = compute_epoch_value(epoch);
      });
   }
}

[[eosio::action]] void drops::addoracle(name oracle)
{
   require_auth(_self);
   check(is_account(oracle), "Account does not exist.");
   drops::oracles_table oracles(_self, _self.value);
   oracles.emplace(_self, [&](auto& row) { row.oracle = oracle; });
}

[[eosio::action]] void drops::removeoracle(name oracle)
{
   require_auth(_self);

   drops::oracles_table oracles(_self, _self.value);
   auto                 oracle_itr = oracles.find(oracle.value);
   check(oracle_itr != oracles.end(), "Oracle not found");
   oracles.erase(oracle_itr);
}

[[eosio::action]] void drops::subscribe(name subscriber)
{
   // TODO: Maybe this needs to require the oracles or _self?
   // The person advancing I think needs to pay for the CPU to notify the other contracts
   require_auth(subscriber);

   drops::subscribers_table subscribers(_self, _self.value);
   auto                     subscriber_itr = subscribers.find(subscriber.value);
   check(subscriber_itr == subscribers.end(), "Already subscribed to notifictions.");
   subscribers.emplace(subscriber, [&](auto& row) { row.subscriber = subscriber; });
}

[[eosio::action]] void drops::unsubscribe(name subscriber)
{
   require_auth(subscriber);

   drops::subscribers_table subscribers(_self, _self.value);
   auto                     subscriber_itr = subscribers.find(subscriber.value);
   check(subscriber_itr != subscribers.end(), "Not currently subscribed.");
   subscribers.erase(subscriber_itr);
}

[[eosio::action]] void drops::enable(bool enabled)
{
   require_auth(_self);

   drops::state_table state(_self, _self.value);
   auto               state_itr = state.find(1);
   state.modify(state_itr, _self, [&](auto& row) { row.enabled = enabled; });
}

[[eosio::action]] void drops::init()
{
   require_auth(_self);

   accounts_table accounts(_self, _self.value);
   epochs_table   epochs(_self, _self.value);
   drop_table     drops(_self, _self.value);
   state_table    state(_self, _self.value);
   stats_table    stats(_self, _self.value);

   accounts.emplace(_self, [&](auto& row) {
      row.account = "eosio"_n;
      row.drops   = 1;
   });

   // Load oracles to initialize the first epoch
   std::vector<name>    oracles;
   drops::oracles_table oracles_table(_self, _self.value);
   auto                 oracle_itr = oracles_table.begin();
   check(oracle_itr != oracles_table.end(), "No oracles registered, cannot init.");
   while (oracle_itr != oracles_table.end()) {
      oracles.push_back(oracle_itr->oracle);
      oracle_itr++;
   }

   // Round epoch timer down to nearest interval to start with
   const time_point_sec epoch =
      time_point_sec((current_time_point().sec_since_epoch() / epochphasetimer) * epochphasetimer);

   epochs.emplace(_self, [&](auto& row) {
      row.epoch     = 1;
      row.start     = epoch;
      row.end       = epoch + eosio::seconds(epochphasetimer);
      row.oracles   = oracles;
      row.completed = 0;
   });

   drops.emplace(_self, [&](auto& row) {
      row.drops = 0;
      row.owner = "eosio"_n;
      row.epoch = 1;
   });

   state.emplace(_self, [&](auto& row) {
      row.id      = 1;
      row.epoch   = 1;
      row.enabled = false;
   });

   stats.emplace(_self, [&](auto& row) {
      row.id      = 1;
      row.account = "eosio"_n;
      row.epoch   = 1;
      row.drops   = 1;
   });
}

[[eosio::action]] void drops::wipe()
{
   require_auth(_self);

   drops::accounts_table accounts(_self, _self.value);
   auto                  account_itr = accounts.begin();
   while (account_itr != accounts.end()) {
      account_itr = accounts.erase(account_itr);
   }

   drops::commits_table commits(_self, _self.value);
   auto                 commit_itr = commits.begin();
   while (commit_itr != commits.end()) {
      commit_itr = commits.erase(commit_itr);
   }

   drops::epochs_table epochs(_self, _self.value);
   auto                epoch_itr = epochs.begin();
   while (epoch_itr != epochs.end()) {
      epoch_itr = epochs.erase(epoch_itr);
   }

   drops::epochdrop_table epochdrops(_self, _self.value);
   auto                   epochdrops_itr = epochdrops.begin();
   while (epochdrops_itr != epochdrops.end()) {
      epochdrops_itr = epochdrops.erase(epochdrops_itr);
   }

   drops::reveals_table reveals(_self, _self.value);
   auto                 reveal_itr = reveals.begin();
   while (reveal_itr != reveals.end()) {
      reveal_itr = reveals.erase(reveal_itr);
   }

   drops::oracles_table oracles(_self, _self.value);
   auto                 oracle_itr = oracles.begin();
   while (oracle_itr != oracles.end()) {
      oracle_itr = oracles.erase(oracle_itr);
   }

   drops::drop_table drops(_self, _self.value);
   auto              drops_itr = drops.begin();
   while (drops_itr != drops.end()) {
      drops_itr = drops.erase(drops_itr);
   }

   drops::state_table state(_self, _self.value);
   auto               state_itr = state.begin();
   while (state_itr != state.end()) {
      state_itr = state.erase(state_itr);
   }

   drops::stats_table stats(_self, _self.value);
   auto               stats_itr = stats.begin();
   while (stats_itr != stats.end()) {
      stats_itr = stats.erase(stats_itr);
   }

   drops::subscribers_table subscribers(_self, _self.value);
   auto                     subscribers_itr = subscribers.begin();
   while (subscribers_itr != subscribers.end()) {
      subscribers_itr = subscribers.erase(subscribers_itr);
   }
}

[[eosio::action]] void drops::wipesome()
{
   require_auth(_self);
   drops::drop_table drops(_self, _self.value);
   auto              drops_itr = drops.begin();

   int i   = 0;
   int max = 10000;
   while (drops_itr != drops.end()) {
      if (i++ > max) {
         break;
      }
      i++;
      drops_itr = drops.erase(drops_itr);
   }
}

std::vector<std::string> drops::split(const std::string& str, char delim)
{
   std::vector<std::string> strings;
   size_t                   start;
   size_t                   end = 0;
   while ((start = str.find_first_not_of(delim, end)) != std::string::npos) {
      end = str.find(delim, start);
      strings.push_back(str.substr(start, end - start));
   }
   return strings;
}

//   ACTION offer(uint64_t drops, name owner, name recipient) {
//     require_auth(owner);

//     offers_table offers(_self, _self.value);
//     drop_table drops(_self, _self.value);

//     // Check to ensure owner owns the drops
//     auto drops_itr = drops.find(drops);
//     check(drops_itr != drops.end(), "Drop not found");
//     check(drops_itr->owner == owner, "Account does not own this drops");

//     // Create the offer
//     offers.emplace(owner, [&](offer_row &row) {
//       row.drops = drops;
//       row.to = recipient;
//     });
//   }

//   ACTION accept(uint64_t drops, name owner) {
//     require_auth(owner);

//     offers_table offers(_self, _self.value);
//     drop_table drops(_self, _self.value);

//     auto offer_itr = offers.find(drops);
//     check(offer_itr != offers.end(), "Offer not found");
//     check(offer_itr->to == owner, "Offer not valid for this account");

//     auto drops_itr = drops.find(drops);
//     check(drops_itr != drops.end(), "Drop not found");

//     // Take over RAM payment and set as new owner
//     drops.modify(drops_itr, owner, [&](auto &row) { row.owner = owner; });

//     // Remove offer once accepted
//     offers.erase(offer_itr);
//   }