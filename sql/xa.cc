/* Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "xa.h"

#include "hash.h"               // HASH
#include "sql_class.h"          // THD
#include "transaction.h"        // trans_begin, trans_rollback
#include "debug_sync.h"         // DEBUG_SYNC
#include "log.h"                // tc_log

static mysql_mutex_t LOCK_xid_cache;
static HASH xid_cache;

const char *XID_STATE::xa_state_names[]={
  "NON-EXISTING", "ACTIVE", "IDLE", "PREPARED", "ROLLBACK ONLY"
};

/* for recover() handlerton call */
static const int MIN_XID_LIST_SIZE= 128;
static const int MAX_XID_LIST_SIZE= 1024*128;


/***************************************************************************
  Handling of XA id caching
***************************************************************************/

/**
  Callback that is called to get the key for a hash.

  @param ptr  pointer to the record
  @param length  length of the record

  @return  pointer to a record stored in cache
*/

extern "C" uchar *xid_get_hash_key(const uchar *ptr, size_t *length,
                                   my_bool not_used __attribute__((unused)))
{
  *length=((XID_STATE*)ptr)->get_xid()->key_length();
  return (uchar*)(((XID_STATE*)ptr)->get_xid()->key());
}


/**
  Callback that is called to do cleanup.

  @param ptr  pointer to free
*/

extern "C" void xid_free_hash(void *ptr)
{
  if (((XID_STATE*)ptr)->is_in_recovery()) // Only time it's malloc'ed
    my_free(ptr);
}


#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_xid_cache;

static PSI_mutex_info all_xid_mutexes[]=
{
  { &key_LOCK_xid_cache, "LOCK_xid_cache", PSI_FLAG_GLOBAL}
};

static void init_xid_psi_keys(void)
{
  int count= array_elements(all_xid_mutexes);
  mysql_mutex_register("sql", all_xid_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */


bool xid_cache_init()
{
#ifdef HAVE_PSI_INTERFACE
  init_xid_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_xid_cache, &LOCK_xid_cache, MY_MUTEX_INIT_FAST);
  return my_hash_init(&xid_cache, &my_charset_bin, 100, 0, 0,
                      xid_get_hash_key, xid_free_hash, 0) != 0;
}


void xid_cache_free()
{
  if (my_hash_inited(&xid_cache))
  {
    my_hash_free(&xid_cache);
    mysql_mutex_destroy(&LOCK_xid_cache);
  }
}


/**
  Search information about XA transaction by a XID value.

  @param xid    Pointer to a XID structure that identifies a XA transaction.

  @return  pointer to a structure that describes XA transaction (XID_STATE).
    @retval  NULL     failure
    @retval  != NULL  success
*/

static XID_STATE *xid_cache_search(const XID *xid)
{
  mysql_mutex_lock(&LOCK_xid_cache);
  XID_STATE *res= (XID_STATE *)my_hash_search(&xid_cache, xid->key(),
                                              xid->key_length());
  mysql_mutex_unlock(&LOCK_xid_cache);
  return res;
}


/**
  Insert information about XA transaction into a cache indexed by XID.

  @param xid     Pointer to a XID structure that identifies a XA transaction.

  @return  operation result
    @retval  false   success or a cache already contains XID_STATE
                     for this XID value
    @retval  true    failure
*/

static bool xid_cache_insert_recovery(const XID *xid)
{
  XID_STATE *xs;
  bool res;
  mysql_mutex_lock(&LOCK_xid_cache);
  if (my_hash_search(&xid_cache, xid->key(), xid->key_length()))
    res= false;
  else if (!(xs= (XID_STATE *)my_malloc(key_memory_XID_STATE,
                                        sizeof(*xs), MYF(MY_WME))))
    res= true;
  else
  {
    xs->start_recovery_xa(xid);
    res=my_hash_insert(&xid_cache, (uchar*)xs);
  }
  mysql_mutex_unlock(&LOCK_xid_cache);
  return res;
}


/**
  Insert information about XA transaction into a cache indexed by XID.

  @param xid_state  Pointer to a XID_STATE structure that describes
                    an XA transaction.

  @return  operation result
    @retval  falase  success or a cache already contains XID_STATE
                     for this XUD value
    @retval  true    failure
*/

static bool xid_cache_insert(const XID_STATE *xid_state)
{
  mysql_mutex_lock(&LOCK_xid_cache);
  if (my_hash_search(&xid_cache, xid_state->get_xid()->key(),
                     xid_state->get_xid()->key_length()))
  {
    mysql_mutex_unlock(&LOCK_xid_cache);
    my_error(ER_XAER_DUPID, MYF(0));
    return true;
  }
  bool res= my_hash_insert(&xid_cache, (const uchar*)xid_state);
  mysql_mutex_unlock(&LOCK_xid_cache);
  return res;
}


void xid_cache_delete(XID_STATE *xid_state)
{
  mysql_mutex_lock(&LOCK_xid_cache);
  my_hash_delete(&xid_cache, (uchar *)xid_state);
  mysql_mutex_unlock(&LOCK_xid_cache);
}


static my_bool xacommit_handlerton(THD *unused1, plugin_ref plugin,
                                   void *arg)
{
  handlerton *hton= plugin_data(plugin, handlerton *);
  if (hton->state == SHOW_OPTION_YES && hton->recover)
    hton->commit_by_xid(hton, (XID *)arg);

  return FALSE;
}


static my_bool xarollback_handlerton(THD *unused1, plugin_ref plugin,
                                     void *arg)
{
  handlerton *hton= plugin_data(plugin, handlerton *);
  if (hton->state == SHOW_OPTION_YES && hton->recover)
    hton->rollback_by_xid(hton, (XID *)arg);

  return FALSE;
}


static void ha_commit_or_rollback_by_xid(THD *thd, XID *xid, bool commit)
{
  plugin_foreach(NULL, commit ? xacommit_handlerton : xarollback_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, xid);

  gtid_rollback(thd);
}


struct xarecover_st
{
  int len, found_foreign_xids, found_my_xids;
  XID *list;
  HASH *commit_list;
  bool dry_run;
};


static my_bool xarecover_handlerton(THD *unused, plugin_ref plugin,
                                    void *arg)
{
  handlerton *hton= plugin_data(plugin, handlerton *);
  struct xarecover_st *info= (struct xarecover_st *) arg;
  int got;

  if (hton->state == SHOW_OPTION_YES && hton->recover)
  {
    while ((got= hton->recover(hton, info->list, info->len)) > 0)
    {
      sql_print_information("Found %d prepared transaction(s) in %s",
                            got, ha_resolve_storage_engine_name(hton));
      for (int i= 0; i < got; i++)
      {
        my_xid x= info->list[i].get_my_xid();
        if (!x) // not "mine" - that is generated by external TM
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
          XID *xid= info->list + i;
          sql_print_information("ignore xid %s", xid->xid_to_str(buf));
#endif
          xid_cache_insert_recovery(info->list + i);
          info->found_foreign_xids++;
          continue;
        }
        if (info->dry_run)
        {
          info->found_my_xids++;
          continue;
        }
        // recovery mode
        if (info->commit_list ?
            my_hash_search(info->commit_list, (uchar *)&x, sizeof(x)) != 0 :
            tc_heuristic_recover == TC_HEURISTIC_RECOVER_COMMIT)
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
          XID *xid= info->list + i;
          sql_print_information("commit xid %s", xid->xid_to_str(buf));
#endif
          hton->commit_by_xid(hton, info->list + i);
        }
        else
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
          XID *xid= info->list + i;
          sql_print_information("rollback xid %s", xid->xid_to_str(buf));
#endif
          hton->rollback_by_xid(hton, info->list + i);
        }
      }
      if (got < info->len)
        break;
    }
  }
  return false;
}


int ha_recover(HASH *commit_list)
{
  struct xarecover_st info;
  DBUG_ENTER("ha_recover");
  info.found_foreign_xids= info.found_my_xids= 0;
  info.commit_list= commit_list;
  info.dry_run= (info.commit_list == 0 && tc_heuristic_recover == 0);
  info.list= NULL;

  /* commit_list and tc_heuristic_recover cannot be set both */
  DBUG_ASSERT(info.commit_list == 0 || tc_heuristic_recover == 0);
  /* if either is set, total_ha_2pc must be set too */
  DBUG_ASSERT(info.dry_run || total_ha_2pc>(ulong)opt_bin_log);

  if (total_ha_2pc <= (ulong)opt_bin_log)
    DBUG_RETURN(0);

  if (info.commit_list)
    sql_print_information("Starting crash recovery...");

  if (total_ha_2pc > (ulong)opt_bin_log + 1)
  {
    if (tc_heuristic_recover == TC_HEURISTIC_RECOVER_ROLLBACK)
    {
      sql_print_error("--tc-heuristic-recover rollback strategy is not safe "
                      "on systems with more than one 2-phase-commit-capable "
                      "storage engine. Aborting crash recovery.");
      DBUG_RETURN(1);
    }
  }
  else
  {
    /*
      If there is only one 2pc capable storage engine it is always safe
      to rollback. This setting will be ignored if we are in automatic
      recovery mode.
    */
    tc_heuristic_recover= TC_HEURISTIC_RECOVER_ROLLBACK; // forcing ROLLBACK
    info.dry_run= false;
  }

  for (info.len= MAX_XID_LIST_SIZE ;
       info.list == 0 && info.len > MIN_XID_LIST_SIZE; info.len/= 2)
  {
    info.list= (XID *)my_malloc(key_memory_XID,
                                info.len * sizeof(XID), MYF(0));
  }
  if (!info.list)
  {
    sql_print_error(ER(ER_OUTOFMEMORY),
                    static_cast<int>(info.len * sizeof(XID)));
    DBUG_RETURN(1);
  }

  plugin_foreach(NULL, xarecover_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &info);

  my_free(info.list);
  if (info.found_foreign_xids)
    sql_print_warning("Found %d prepared XA transactions",
                      info.found_foreign_xids);
  if (info.dry_run && info.found_my_xids)
  {
    sql_print_error("Found %d prepared transactions! It means that mysqld was "
                    "not shut down properly last time and critical recovery "
                    "information (last binlog or %s file) was manually deleted"
                    " after a crash. You have to start mysqld with "
                    "--tc-heuristic-recover switch to commit or rollback "
                    "pending transactions.",
                    info.found_my_xids, opt_tc_log_file);
    DBUG_RETURN(1);
  }
  if (info.commit_list)
    sql_print_information("Crash recovery finished.");
  DBUG_RETURN(0);
}


/**
  Rollback the active XA transaction.

  @note Resets rm_error before calling ha_rollback(), so
        the thd->transaction.xid structure gets reset
        by ha_rollback() / THD::transaction::cleanup().

  @return true if the rollback failed, false otherwise.
*/

static bool xa_trans_force_rollback(THD *thd)
{
  /*
    We must reset rm_error before calling ha_rollback(),
    so thd->transaction.xid structure gets reset
    by ha_rollback()/THD::transaction::cleanup().
  */
  thd->transaction.xid_state.reset_error();
  if (ha_rollback_trans(thd, true))
  {
    my_error(ER_XAER_RMERR, MYF(0));
    return true;
  }
  return false;
}


bool trans_xa_commit(THD *thd)
{
  bool res= true;
  XID_STATE *xid_state= &thd->transaction.xid_state;
  DBUG_ENTER("trans_xa_commit");

  if (!xid_state->has_same_xid(thd->lex->xid))
  {
    /*
      Note, that there is no race condition here between xid_cache_search
      and xid_cache_delete, since we always delete our own XID
      (thd->lex->xid == thd->transaction.xid_state.xid).
      The only case when thd->lex->xid != thd->transaction.xid_state.xid
      and xid_state->in_thd == 0 is in the function
      xid_cache_insert_recovery(XID), which is called before starting
      client connections, and thus is always single-threaded.
    */
    XID_STATE *xs= xid_cache_search(thd->lex->xid);
    res= !xs || !xs->is_in_recovery();
    if (res)
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      res= xs->xa_trans_rolled_back();
      ha_commit_or_rollback_by_xid(thd, thd->lex->xid, !res);
      xid_cache_delete(xs);
    }
    DBUG_RETURN(res);
  }

  if (xid_state->xa_trans_rolled_back())
  {
    xa_trans_force_rollback(thd);
    res= thd->is_error();
  }
  else if (xid_state->has_state(XID_STATE::XA_IDLE) &&
           thd->lex->xa_opt == XA_ONE_PHASE)
  {
    int r= ha_commit_trans(thd, true);
    if ((res= MY_TEST(r)))
      my_error(r == 1 ? ER_XA_RBROLLBACK : ER_XAER_RMERR, MYF(0));
  }
  else if (xid_state->has_state(XID_STATE::XA_PREPARED) &&
           thd->lex->xa_opt == XA_NONE)
  {
    MDL_request mdl_request;

    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    MDL_REQUEST_INIT(&mdl_request,
                     MDL_key::COMMIT, "", "", MDL_INTENTION_EXCLUSIVE,
                     MDL_TRANSACTION);

    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
    {
      ha_rollback_trans(thd, true);
      my_error(ER_XAER_RMERR, MYF(0));
    }
    else
    {
      DEBUG_SYNC(thd, "trans_xa_commit_after_acquire_commit_lock");

      if (tc_log)
        res= MY_TEST(tc_log->commit(thd, /* all */ true));
      else
        res= MY_TEST(ha_commit_low(thd, /* all */ true));

      if (res)
        my_error(ER_XAER_RMERR, MYF(0));
    }
  }
  else
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    DBUG_RETURN(true);
  }

  thd->variables.option_bits&= ~OPTION_BEGIN;
  thd->transaction.all.reset_unsafe_rollback_flags();
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(xid_state);
  xid_state->set_state(XID_STATE::XA_NOTR);

  DBUG_RETURN(res);
}


bool trans_xa_rollback(THD *thd)
{
  XID_STATE *xid_state= &thd->transaction.xid_state;
  DBUG_ENTER("trans_xa_rollback");

  if (!xid_state->has_same_xid(thd->lex->xid))
  {
    XID_STATE *xs= xid_cache_search(thd->lex->xid);
    if (!xs || !xs->is_in_recovery())
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      xs->xa_trans_rolled_back();
      ha_commit_or_rollback_by_xid(thd, thd->lex->xid, false);
      xid_cache_delete(xs);
    }
    DBUG_RETURN(thd->is_error());
  }

  if (xid_state->has_state(XID_STATE::XA_NOTR) ||
      xid_state->has_state(XID_STATE::XA_ACTIVE))
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    DBUG_RETURN(true);
  }

  bool res= xa_trans_force_rollback(thd);

  thd->variables.option_bits&= ~OPTION_BEGIN;
  thd->transaction.all.reset_unsafe_rollback_flags();
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(xid_state);
  xid_state->set_state(XID_STATE::XA_NOTR);

  DBUG_RETURN(res);
}


bool trans_xa_start(THD *thd)
{
  XID_STATE *xid_state= &thd->transaction.xid_state;
  DBUG_ENTER("trans_xa_start");

  if (xid_state->has_state(XID_STATE::XA_IDLE) &&
      thd->lex->xa_opt == XA_RESUME)
  {
    bool not_equal= !xid_state->has_same_xid(thd->lex->xid);
    if (not_equal)
      my_error(ER_XAER_NOTA, MYF(0));
    else
      xid_state->set_state(XID_STATE::XA_ACTIVE);
    DBUG_RETURN(not_equal);
  }

  /* TODO: JOIN is not supported yet. */
  if (thd->lex->xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!xid_state->has_state(XID_STATE::XA_NOTR))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
    my_error(ER_XAER_OUTSIDE, MYF(0));
  else if (!trans_begin(thd))
  {
    xid_state->start_normal_xa(thd->lex->xid);
    if (xid_cache_insert(xid_state))
    {
      xid_state->reset();
      trans_rollback(thd);
    }
  }

  DBUG_RETURN(thd->is_error() ||
              !xid_state->has_state(XID_STATE::XA_ACTIVE));
}


bool trans_xa_end(THD *thd)
{
  XID_STATE *xid_state= &thd->transaction.xid_state;
  DBUG_ENTER("trans_xa_end");

  /* TODO: SUSPEND and FOR MIGRATE are not supported yet. */
  if (thd->lex->xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!xid_state->has_state(XID_STATE::XA_ACTIVE))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (!xid_state->has_same_xid(thd->lex->xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (!xid_state->xa_trans_rolled_back())
    xid_state->set_state(XID_STATE::XA_IDLE);

  DBUG_RETURN(thd->is_error() ||
              !xid_state->has_state(XID_STATE::XA_IDLE));
}


bool trans_xa_prepare(THD *thd)
{
  XID_STATE *xid_state= &thd->transaction.xid_state;
  DBUG_ENTER("trans_xa_prepare");

  if (!xid_state->has_state(XID_STATE::XA_IDLE))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (!xid_state->has_same_xid(thd->lex->xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (ha_prepare(thd))
  {
    xid_cache_delete(xid_state);
    xid_state->set_state(XID_STATE::XA_NOTR);
    my_error(ER_XA_RBROLLBACK, MYF(0));
  }
  else
    xid_state->set_state(XID_STATE::XA_PREPARED);

  DBUG_RETURN(thd->is_error() ||
              !xid_state->has_state(XID_STATE::XA_PREPARED));
}


bool trans_xa_recover(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  int i= 0;
  XID_STATE *xs;
  DBUG_ENTER("trans_xa_recover");

  field_list.push_back(new Item_int(NAME_STRING("formatID"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_int(NAME_STRING("gtrid_length"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_int(NAME_STRING("bqual_length"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_empty_string("data", XIDDATASIZE*2+2));

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(true);

  mysql_mutex_lock(&LOCK_xid_cache);
  while ((xs= (XID_STATE*) my_hash_element(&xid_cache, i++)))
  {
    if (xs->has_state(XID_STATE::XA_PREPARED))
    {
      protocol->prepare_for_resend();
      xs->store_xid_info(protocol);

      if (protocol->write())
      {
        mysql_mutex_unlock(&LOCK_xid_cache);
        DBUG_RETURN(true);
      }
    }
  }

  mysql_mutex_unlock(&LOCK_xid_cache);
  my_eof(thd);
  DBUG_RETURN(false);
}


bool XID_STATE::xa_trans_rolled_back()
{
  if (rm_error)
  {
    switch (rm_error)
    {
    case ER_LOCK_WAIT_TIMEOUT:
      my_error(ER_XA_RBTIMEOUT, MYF(0));
      break;
    case ER_LOCK_DEADLOCK:
      my_error(ER_XA_RBDEADLOCK, MYF(0));
      break;
    default:
      my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    xa_state= XID_STATE::XA_ROLLBACK_ONLY;
  }

  return (xa_state == XID_STATE::XA_ROLLBACK_ONLY);
}


bool XID_STATE::check_xa_idle_or_prepared(bool report_error) const
{
  if (xa_state == XA_IDLE ||
      xa_state == XA_PREPARED)
  {
    if (report_error)
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);

    return true;
  }

  return false;
}


bool XID_STATE::check_has_uncommitted_xa() const
{
  if (xa_state == XA_IDLE ||
      xa_state == XA_PREPARED ||
      xa_state == XA_ROLLBACK_ONLY)
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    return true;
  }

  return false;
}


bool XID_STATE::check_in_xa(bool report_error) const
{
  if (xa_state != XA_NOTR)
  {
    if (report_error)
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    return true;
  }

  return false;
}


void XID_STATE::set_error(THD *thd)
{
  if (xa_state != XA_NOTR)
    rm_error= thd->get_stmt_da()->mysql_errno();
}


void XID_STATE::store_xid_info(Protocol *protocol) const
{
  protocol->store_longlong(static_cast<longlong>(m_xid.formatID), false);
  protocol->store_longlong(static_cast<longlong>(m_xid.gtrid_length), false);
  protocol->store_longlong(static_cast<longlong>(m_xid.bqual_length), false);

  if (m_xid.is_printable_xid() == true)
  {
    protocol->store(m_xid.data, m_xid.gtrid_length + m_xid.bqual_length,
                    &my_charset_bin);
  }
  else
  {
    /*
      xid_buf contains enough space for 0x followed by HEX representation
      of the binary XID data and one null termination character.
    */
    char xid_buf[XIDDATASIZE * 2 + 2 + 1];

    xid_buf[0]= '0';
    xid_buf[1]= 'x';

    uint xid_str_len= bin_to_hex_str(xid_buf + 2, sizeof(xid_buf) - 2,
                                     const_cast<char*>(m_xid.data),
                                     m_xid.gtrid_length +
                                     m_xid.bqual_length) + 2;
    protocol->store(xid_buf, xid_str_len, &my_charset_bin);
  }
}


#ifndef DBUG_OFF
char* XID::xid_to_str(char *buf) const
{
  char *s= buf;
  *s++= '\'';

  for (int i= 0; i < gtrid_length + bqual_length; i++)
  {
    /* is_next_dig is set if next character is a number */
    bool is_next_dig= false;
    if (i < XIDDATASIZE)
    {
      char ch= data[i+1];
      is_next_dig= (ch >= '0' && ch <='9');
    }
    if (i == gtrid_length)
    {
      *s++= '\'';
      if (bqual_length)
      {
        *s++= '.';
        *s++= '\'';
      }
    }
    uchar c= static_cast<uchar>(data[i]);
    if (c < 32 || c > 126)
    {
      *s++= '\\';
      /*
        If next character is a number, write current character with
        3 octal numbers to ensure that the next number is not seen
        as part of the octal number
      */
      if (c > 077 || is_next_dig)
        *s++= _dig_vec_lower[c >> 6];
      if (c > 007 || is_next_dig)
        *s++=_dig_vec_lower[(c >> 3) & 7];
      *s++= _dig_vec_lower[c & 7];
    }
    else
    {
      if (c == '\'' || c == '\\')
        *s++= '\\';
      *s++= c;
    }
  }
  *s++= '\'';
  *s= 0;
  return buf;
}
#endif


bool XID::is_printable_xid() const
{
  for (int i= 0; i < gtrid_length + bqual_length; i++)
  {
    uchar c= static_cast<uchar>(data[i]);
    if (c < 32 || c > 127)
      return false;
  }

  return true;
}