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
 *  CohortsENQ Implementation
 *
 *  CohortsENQ is CohortsNorec with inplace write if I'm the last one in the
 *  cohort. Use queue to handle ordered commit.
 */

#include "../profiling.hpp"
#include "../algs.hpp"
#include "../RedoRAWUtils.hpp"

using stm::TxThread;
using stm::WriteSet;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;
using stm::ValueList;
using stm::ValueListEntry;
using stm::started;
using stm::cohorts_node_t;

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  volatile uintptr_t inplace = 0;

  NOINLINE bool validate(TxThread* tx);

  // global linklist's head
  struct cohorts_node_t* volatile q = NULL;

  struct CohortsENQ {
      static void begin(TX_LONE_PARAMETER);
      static TM_FASTCALL void* read_ro(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void* read_rw(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void* read_turbo(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_turbo(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void commit_ro(TX_LONE_PARAMETER);
      static TM_FASTCALL void commit_rw(TX_LONE_PARAMETER);
      static TM_FASTCALL void commit_turbo(TX_LONE_PARAMETER);

      static void rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
  };

  /**
   *  CohortsENQ begin:
   *  CohortsENQ has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  void CohortsENQ::begin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();
    S1:
      // wait until everyone is committed
      while (q != NULL);

      // before tx begins, increase total number of tx
      faiptr(&started.val);

      // [NB] we must double check no one is ready to commit yet
      // and no one entered in place write phase(turbo mode)
      if (q != NULL|| inplace == 1){
          faaptr(&started.val, -1);
          goto S1;
      }

      // reset local turn val
      tx->turn.val = COHORTS_NOTDONE;
  }

  /**
   *  CohortsENQ commit (read-only):
   */
  void
  CohortsENQ::commit_ro(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // decrease total number of tx started
      faaptr(&started.val, -1);

      // clean up
      tx->vlist.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  CohortsENQ commit (in place write commit): no validation, no write back
   *  no other thread touches cpending
   */
  void
  CohortsENQ::commit_turbo(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // decrease total number of tx started
      faaptr(&started.val, -1);

      // clean up
      tx->vlist.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);

      // wait for tx in commit_rw finish
      while (q != NULL);

      // reset in place write flag
      inplace = 0;
  }

  /**
   *  CohortsENQ commit (writing context):
   *
   *  RW commit is operated in turns. Transactions will be allowed to commit
   *  in an order which is given at the beginning of commit.
   */
  void
  CohortsENQ::commit_rw(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // add myself to the queue
      do {
          tx->turn.next = q;
      }while (!bcasptr(&q, tx->turn.next, &(tx->turn)));

      // decrease total number of tx started
      faaptr(&started.val, -1);

      // wait for my turn
      if (tx->turn.next != NULL)
          while (tx->turn.next->val != COHORTS_DONE);

      // Wait until all tx are ready to commit
      while (started.val != 0);

      // If in place write occurred, all tx validate reads
      // Otherwise, only first one skips validation
      if (inplace == 1 || tx->turn.next != NULL)
          if (!validate(tx)) {
              // mark self done
              tx->turn.val = COHORTS_DONE;
              // reset q if last one
              if (q == &(tx->turn)) q = NULL;
              // abort
              stm::tmabort();
          }

      // do write back
      tx->writes.writeback();
      CFENCE;

      // mark self as done
      tx->turn.val = COHORTS_DONE;

      // last one in cohort reset q
      if (q == &(tx->turn))
          q = NULL;

      // commit all frees, reset all lists
      tx->vlist.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  CohortsENQ read_turbo
   */
  void*
  CohortsENQ::read_turbo(TX_FIRST_PARAMETER_ANON STM_READ_SIG(addr,))
  {
      return *addr;
  }

  /**
   *  CohortsENQ read (read-only transaction)
   */
  void*
  CohortsENQ::read_ro(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      void *tmp = *addr;
      STM_LOG_VALUE(tx, addr, tmp, mask);
      return tmp;
  }

  /**
   *  CohortsENQ read (writing transaction)
   */
  void*
  CohortsENQ::read_rw(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      void* tmp = *addr;
      STM_LOG_VALUE(tx, addr, tmp, mask);
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      return tmp;
  }

  /**
   *  CohortsENQ write (read-only context): for first write
   */
  void
  CohortsENQ::write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // If everyone else is ready to commit, do in place write
      if (started.val == 1) {
          // set up flag indicating in place write starts
          atomicswapptr(&inplace, 1);
          // double check is necessary
          if (started.val == 1) {
              // in place write
              *addr = val;
              // go turbo mode
              stm::OnFirstWrite(tx, read_turbo, write_turbo, commit_turbo);
              return;
          }
          // reset flag
          inplace = 0;
      }
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      stm::OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  CohortsENQ write (in place write)
   */
  void
  CohortsENQ::write_turbo(TX_FIRST_PARAMETER_ANON STM_WRITE_SIG(addr,val,mask))
  {
      *addr = val; // in place write
  }

  /**
   *  CohortsENQ write (writing context)
   */
  void
  CohortsENQ::write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  CohortsENQ unwinder:
   */
  void
  CohortsENQ::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // reset all lists
      tx->vlist.reset();
      tx->writes.reset();

      PostRollback(tx);
  }

  /**
   *  CohortsENQ in-flight irrevocability:
   */
  bool
  CohortsENQ::irrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsENQ Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsENQ validation for commit: check that all reads are valid
   */
  bool
  validate(TxThread* tx)
  {
      foreach (ValueList, i, tx->vlist) {
          bool valid = STM_LOG_VALUE_IS_VALID(i, tx);
          if (!valid) return false;
      }
      return true;
  }

  /**
   *  Switch to CohortsENQ:
   *
   */
  void
  CohortsENQ::onSwitchTo()
  {
      inplace = 0;
  }
}

namespace stm {
  /**
   *  CohortsENQ initialization
   */
  template<>
  void initTM<CohortsENQ>()
  {
      // set the name
      stms[CohortsENQ].name      = "CohortsENQ";
      // set the pointers
      stms[CohortsENQ].begin     = ::CohortsENQ::begin;
      stms[CohortsENQ].commit    = ::CohortsENQ::commit_ro;
      stms[CohortsENQ].read      = ::CohortsENQ::read_ro;
      stms[CohortsENQ].write     = ::CohortsENQ::write_ro;
      stms[CohortsENQ].rollback  = ::CohortsENQ::rollback;
      stms[CohortsENQ].irrevoc   = ::CohortsENQ::irrevoc;
      stms[CohortsENQ].switcher  = ::CohortsENQ::onSwitchTo;
      stms[CohortsENQ].privatization_safe = true;
  }
}


#ifdef STM_ONESHOT_ALG_CohortsENQ
DECLARE_AS_ONESHOT_TURBO(CohortsENQ)
#endif