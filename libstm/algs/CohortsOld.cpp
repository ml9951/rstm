/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

/**
 *  Cohorts Implementation
 *
 *  Cohorts has 4 stages. 1) Nobody is running. If anyone starts,
 *  goes to 2) Everybody is running. If anyone is ready to commit,
 *  goes to 3) Every rw tx gets an order, from now on, no one is
 *  allowed to start a tx anymore. When everyone in this cohort is
 *  ready to commit, goes to stage 4)Commit phase. Everyone commits
 *  in an order that given in stage 3. When the last one finishes
 *  its commit, it goes to stage 1. Now tx is allowed to start again.
 */

#include "algs.hpp"
#include "../Diagnostics.hpp"

namespace stm
{
  TM_FASTCALL void* CohortsOldReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* CohortsOldReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void CohortsOldWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void CohortsOldWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void CohortsOldCommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void CohortsOldCommitRW(TX_LONE_PARAMETER);
  NOINLINE void CohortsOldValidate(TxThread* tx, uintptr_t finish_cache);
  NOINLINE void CohortsOldValidateCM(TxThread* tx, uintptr_t finish_cache);
  NOINLINE void CohortsOldTxAbortWrapper(TxThread* tx);
  NOINLINE void CohortsOldTxAbortWrapperCM(TxThread* tx);

  /**
   *  CohortsOld begin:
   *  CohortsOld has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  void CohortsOldBegin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // wait until we are allowed to start
      // when started is even, we wait
      while (started.val % 2 == 0){
          // unless started is 0, which means all commits is done
          if (started.val == 0)
          {
              // set no validation, for big lock
              locks[0] = 0;

              // now we can start again
              casptr(&started.val, 0, -1);
          }

          // check if an adaptivity action is underway
          if (tmbegin != CohortsOldBegin){
              tmabort();
          }
      }

      CFENCE;
      // before start, increase total number of tx in one cohort
      faaptr(&started.val, 2);

      tx->allocator.onTxBegin();
      // get time of last finished txn
      tx->ts_cache = last_complete.val;
  }

  /**
   *  CohortsOld commit (read-only):
   *  RO commit is easy.
   */
  void
  CohortsOldCommitRO(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // decrease total number of tx in a cohort
      faaptr(&started.val, -2);

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      OnROCommit(tx);

  }

  /**
   *  CohortsOld commit (writing context):
   *
   *  RW commit is operated in turns. Transactions will be allowed to commit
   *  in an order which is given at the beginning of commit.
   */
  void
  CohortsOldCommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // NB: get a new order at the begainning of commit
      tx->order = 1 + faiptr(&timestamp.val);

      // Wait until it is our turn to commit, validate, and do writeback
      while (last_complete.val != (uintptr_t)(tx->order - 1)) {
          if (tmbegin != CohortsOldBegin)
              CohortsOldTxAbortWrapperCM(tx);
      }

      // since we have order, from now on ,only one tx can go through below at one time

      // started is odd, so I'm the first to enter commit in a cohort
      if (started.val % 2 != 0)
      {
          // set started from odd to even, so that no one can begin now
          faiptr(&started.val);

          // set validation flag
          casptr(&locks[0], 0, 1); // we need validations in read from now on

          // wait until all the small locks are unlocked
          for(uint32_t i = 1; i < 9 ; i++)
              while(locks[i] != 0);

      }

      // since we have the token, we can validate before getting locks
      CohortsOldValidateCM(tx, last_complete.val);

      // if we had writes, then aborted, then restarted, and then didn't have
      // writes, we could end up trying to lock a nonexistant write set.  This
      // condition prevents that case.
      if (tx->writes.size() != 0) {
          // mark every location in the write set, and do write-back
          foreach (WriteSet, i, tx->writes) {
              // get orec
              orec_t* o = get_orec(i->addr);
              // mark orec
              o->v.all = tx->order;
              CFENCE;
              // WBW
              // write-back
              *i->addr = i->val;
          }
      }

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, CohortsOldReadRO, CohortsOldWriteRO, CohortsOldCommitRO);

      // decrease total number of committing tx
      faaptr(&started.val, -2);

      // mark self as done
      last_complete.val = tx->order;

      // set status to committed...
      tx->order = -1;
  }

  /**
   *  CohortsOld read (read-only transaction)
   *  Standard orec read function.
   */
  void*
  CohortsOldReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      void* tmp = *addr;
      CFENCE; // RBR between dereference and orec check

      // It's possible that no validation is needed
      if (started.val % 2 != 0 && locks[0] == 0)
      {
          // mark my lock 1, means I'm doing no validation ReadRO
          locks[tx->id] = 1;

          if (locks[0] == 0)
          {
              orec_t* o = get_orec(addr);
              // log orec
              tx->r_orecs.insert(o);

              // update the finish_cache to remember that at this time, we were valid
              if (last_complete.val > tx->ts_cache)
                  tx->ts_cache = last_complete.val;

              // mark my lock 0, means I finished no validation ReadRO
              locks[tx->id] = 0;
              return tmp;
          }
          else
              // mark my lock 0, means I will do validation ReadRO
              locks[tx->id] = 0;

      }

      // get the orec addr, read the orec's version#
      orec_t* o = get_orec(addr);
      uintptr_t ivt = o->v.all;
      // abort if this changed since the last time I saw someone finish
      //
      // NB: this is a pretty serious tradeoff... it admits false aborts for
      //     the sake of preventing a 'check if locked' test
      if (ivt > tx->ts_cache){
          CohortsOldTxAbortWrapper(tx);
      }

      // log orec
      tx->r_orecs.insert(o);

      // validate
      if (last_complete.val > tx->ts_cache)
          CohortsOldValidate(tx, last_complete.val);

      return tmp;
  }

  /**
   *  CohortsOld read (writing transaction)
   */
  void*
  CohortsOldReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // reuse the ReadRO barrier, which is adequate here---reduces LOC
      void* val = CohortsOldReadRO(TX_FIRST_ARG addr STM_MASK(mask));

      REDO_RAW_CLEANUP(tmp, found, log, mask);
      return val;
  }

  /**
   *  CohortsOld write (read-only context)
   */
  void
  CohortsOldWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, CohortsOldReadRW, CohortsOldWriteRW, CohortsOldCommitRW);
  }

  /**
   *  CohortsOld write (writing context)
   */
  void
  CohortsOldWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  CohortsOld unwinder:
   */
  void
  CohortsOldRollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // reset all lists, but keep any order we acquired
      tx->r_orecs.reset();
      tx->writes.reset();
      // NB: we can't reset pointers here, because if the transaction
      //     performed some writes, then it has an order.  If it has an
      //     order, but restarts and is read-only, then it still must call
      //     CommitRW to finish in-order

      PostRollback(tx);
  }

  /**
   *  CohortsOld in-flight irrevocability:
   */
  bool
  CohortsOldIrrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsOld Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsOld validation
   */
  void
  CohortsOldValidate(TxThread* tx, uintptr_t finish_cache)
  {
      // check that all reads are valid
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // if it has a timestamp of ts_cache or greater, abort
          if (ivt > tx->ts_cache)
              CohortsOldTxAbortWrapper(tx);
      }
      // now update the finish_cache to remember that at this time, we were
      // still valid
      tx->ts_cache = finish_cache;
  }

  /**
   *  CohortsOld validation for commit
   */
  void
  CohortsOldValidateCM(TxThread* tx, uintptr_t finish_cache)
  {
      // check that all reads are valid
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // if it has a timestamp of ts_cache or greater, abort
          if (ivt > tx->ts_cache)
              CohortsOldTxAbortWrapperCM(tx);
      }
      // now update the finish_cache to remember that at this time, we were
      // still valid
      tx->ts_cache = finish_cache;
  }

  /**
   *   CohortsOld Tx Abort Wrapper
   *   decrease total # in one cohort, and abort
   */
  void
  CohortsOldTxAbortWrapper(TxThread*)
  {
      // decrease total number of tx in one cohort
      faaptr(&started.val, -2);

      // abort
      tmabort();
  }

  /**
   *   CohortsOld Tx Abort Wrapper for commit
   *   for abort inside commit. Since we already have order, we need to mark
   *   self as last_complete, and decrease total number of tx in one cohort.
   */
  void
  CohortsOldTxAbortWrapperCM(TxThread* tx)
  {
      // decrease total number of tx in one cohort
      faaptr(&started.val, -2);

      // set self as completed
      last_complete.val = tx->order;

      // abort
      tmabort();
  }

  /**
   *  Switch to CohortsOld:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   *    Also, last_complete must equal timestamp
   *
   *    Also, all threads' order values must be -1
   */
  void
  CohortsOldOnSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;

      // init total tx number in an cohort
      started.val = -1;

      for (uint32_t i = 0; i < threadcount.val; ++i)
          threads[i]->order = -1;

      // unlock all the locks
      for (uint32_t i = 0; i < 9; i++)
          locks[i] = 0;
  }
}


DECLARE_SIMPLE_METHODS_FROM_NORMAL(CohortsOld)
REGISTER_FGADAPT_ALG(CohortsOld, "CohortsOld", true)

#ifdef STM_ONESHOT_ALG_CohortsOld
DECLARE_AS_ONESHOT(CohortsOld)
#endif
