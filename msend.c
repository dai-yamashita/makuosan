/*
 * msend.c
 * Copyright (C) 2008-2012 KLab Inc. 
 */
#include "makuosan.h"

/******************************************************************
*
* send common functions (private)
*
*******************************************************************/
static mfile *msend_mfdel(mfile *m)
{
  mfile *r;
  if(!m){
    return(NULL);
  }
  r = m->next;
  if(m->fd != -1){
    close(m->fd);
  }
  if(m->pipe != -1){
    close(m->pipe);
  }
  if(m->pid){
    kill(m->pid, SIGTERM);
    waitpid(m->pid, NULL, 0);
  }
  if(m->link){
    m->link->link = NULL;
    m->link = NULL;
  }
  while((m->mark = delmark(m->mark)));
  clr_hoststate(m);
  mfdel(m);
  return(r);
}

static int msend_encrypt(mdata *data)
{
  int  szdata;
  MD5_CTX ctx;

  szdata = data->head.szdata;
  if(moption.cryptena){
    data->head.flags |= MAKUO_FLAG_CRYPT;
    if(data->head.szdata){
      MD5_Init(&ctx);
      MD5_Update(&ctx, data->data, data->head.szdata);
      MD5_Final(data->head.hash, &ctx);
      for(szdata=0;szdata<data->head.szdata;szdata+=8){
        BF_encrypt((BF_LONG *)(data->data + szdata), &EncKey);
      }
    }
  }
  return(szdata);
}

static int msend_readywait()
{
  fd_set fds;
  time_t tm;
  struct timeval tv;

  if(moption.sendrate){
    tm = time(NULL);
    while(tm == send_time){
      if(moption.sendrate > send_rate){
        break;
      }
      usleep(1000);
      tm = time(NULL);
    }
    if(tm != send_time){
      view_rate = send_rate;
      send_rate =  0;
      send_time = tm;
    }
  }
  while(!moption.sendready){
    FD_ZERO(&fds);
    FD_SET(moption.mcsocket, &fds);
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    if(select(1024, NULL, &fds, NULL, &tv) == 1){
      moption.sendready = FD_ISSET(moption.mcsocket, &fds);
    }else{
      if(loop_flag){
        continue;
      }else{
        break;
      }
    }
  }
  return(moption.sendready);
}

static int msend_packet(int s, mdata *data, struct sockaddr_in *addr)
{
  int r;
  int szdata;
  mdata senddata;

  memcpy(&senddata, data, sizeof(senddata));
  szdata = msend_encrypt(&senddata);

  senddata.head.szdata = htons(senddata.head.szdata);
  senddata.head.flags  = htons(senddata.head.flags);
  senddata.head.reqid  = htonl(senddata.head.reqid);
  senddata.head.seqno  = htonl(senddata.head.seqno);
  senddata.head.maddr  = senddata.head.maddr;
  senddata.head.mport  = senddata.head.mport;
  senddata.head.error  = htonl(senddata.head.error);
  szdata += sizeof(mhead);
 
  while(msend_readywait()){ 
    moption.sendready = 0;
    r = sendto(s, &senddata, szdata, 0, (struct sockaddr*)addr, sizeof(struct sockaddr_in));
    if(r == szdata){
      send_rate += r;
      return(0); /* success */
    }
    if(r == -1){
      if(errno == EINTR){
        continue;
      }
      lprintf(0,"[error] %s: send error (%s) %s %s rid=%d size=%d seqno=%d\n",
        __func__,
        strerror(errno), 
        stropcode(data), 
        strmstate(data),
        data->head.reqid, 
        szdata, 
        data->head.seqno);
      break;
    }else{
      lprintf(0, "[error] %s: send size error %s %s rid=%d datasize=%d sendsize=%d seqno=%d\n",
        __func__,
        stropcode(data), 
        strmstate(data),
        data->head.reqid,
        szdata, 
        r, 
        data->head.seqno);
      break;
    }
  }
  return(-1);
}

/* retry */
static int msend_retry(mfile *m)
{
  uint8_t *r;
  mhost   *t;

  if(!m){
    return(-1);
  }
  if(!m->sendwait){
    m->retrycnt = MAKUO_SEND_RETRYCNT;
    return(0);
  }
  if(moption.loglevel > 1){
    mprintf(2, __func__, m); 
    for(t=members;t;t=t->next){
      r = get_hoststate(t, m);
      if(!r){
        lprintf(0, "[error] %s: can't alloc state area %s\n", __func__, t->hostname);
        continue;
      }
      if(m->sendto){
        if(!memcmp(&(m->addr.sin_addr), &(t->ad), sizeof(t->ad))){
          if(*r == MAKUO_RECVSTATE_NONE){
            lprintf(2, "%s:   %s %s(%s)\n", __func__, strrstate(*r), inet_ntoa(t->ad), t->hostname);
          }
        }
      }else{
        switch(moption.loglevel){
          case 2:
            if(*r == MAKUO_RECVSTATE_NONE){
              lprintf(2, "%s:   %s %s(%s)\n", __func__, strrstate(*r), inet_ntoa(t->ad), t->hostname);
            }
            break;
          default:
            lprintf(3, "%s:   %s %s(%s)\n", __func__, strrstate(*r), inet_ntoa(t->ad), t->hostname);
            break;
        }
      }
    }
  }
  m->retrycnt--;
  return(0);
}

/* send & free */
static void msend_shot(int s, mfile *m)
{
  if(!msend_packet(s, &(m->mdata), &(m->addr))){
    msend_mfdel(m);
  }
}

/******************************************************************
*
* ack send functions (for destination node tasks)
*
*******************************************************************/
static void msend_ack_ping(int s, mfile *m)
{
  msend_shot(s, m);
}

static void msend_ack_send(int s, mfile *m)
{
  msend_shot(s, m);
}

static void msend_ack_md5(int s, mfile *m)
{
  int r;
  unsigned char hash[16];
  unsigned char buff[8192];
  mfile *d = m->link;
  if(!d){
    msend_shot(s, m);
    return;
  }
  r = read(d->fd, buff, sizeof(buff));
  if(r > 0){
    MD5_Update(&(d->md5), buff, r);
    return;
  }
  if(r == -1){
    if(errno == EINTR){
      return;
    }
    d->mdata.head.error  = errno;
    d->mdata.head.nstate = MAKUO_RECVSTATE_READERROR;
    lprintf(0, "[error] %s: file read error %s\n", __func__, d->fn);
    MD5_Final(hash, &(d->md5));
  }else{
    MD5_Final(hash, &(d->md5));
    if(!memcmp(hash, d->mdata.data, 16)){
      d->mdata.head.nstate = MAKUO_RECVSTATE_MD5OK;
    }else{
      d->mdata.head.nstate = MAKUO_RECVSTATE_MD5NG;
    }
  }
  m->mdata.head.error  = d->mdata.head.error;
  m->mdata.head.nstate = d->mdata.head.nstate;
  close(d->fd);
  d->fd = -1;
  d->link = NULL;
  m->link = NULL;
  msend_shot(s, m);
}

static void msend_ack_dsync(int s, mfile *m)
{
  msend_shot(s, m);
}

static void msend_ack_del(int s, mfile *m)
{
  msend_shot(s, m);
}

static void msend_ack(int s, mfile *m)
{
  switch(m->mdata.head.opcode){
    case MAKUO_OP_PING:
      msend_ack_ping(s, m);
      break;
    case MAKUO_OP_EXIT:
      break;
    case MAKUO_OP_SEND:
      msend_ack_send(s, m);
      break;
    case MAKUO_OP_MD5:
      msend_ack_md5(s, m);
      break;
    case MAKUO_OP_DSYNC:
      msend_ack_dsync(s, m);
      break;
    case MAKUO_OP_DEL:
      msend_ack_del(s, m);
      break;
    /* 機能追加はここ */
  }
}

/******************************************************************
*
* req send functions (for source node tasks)
*
*******************************************************************/
static void msend_req_send_break_init(int s, mfile *m)
{
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, -1);
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_send_break(int s, mfile *m)
{
  if(m->initstate){
    msend_req_send_break_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  msend_mfdel(m);
}

static void msend_req_send_stat_init(int s, mfile *m)
{
  mstat    fs;
  uint64_t dev;

  if(!m->comm){
    msend_mfdel(m);
    m = NULL;
    return;
  }

  m->mdata.p = m->mdata.data;
  m->mdata.head.szdata  = sizeof(fs);
  m->mdata.head.szdata += strlen(m->fn);
  m->mdata.head.szdata += strlen(m->ln);
  m->mdata.head.szdata += sizeof(uint64_t);
  if(m->mdata.head.szdata > MAKUO_BUFFER_SIZE){
    lprintf(0, "[error] %s: buffer size over size=%d file=%s\n",   __func__, m->mdata.head.szdata, m->fn);
    cprintf(0, m->comm, "error: buffer size over size=%d file=%s\n", m->mdata.head.szdata, m->fn);
    return;
  }
  fs.mode  = htonl(m->fs.st_mode);
  fs.uid   = htons(m->fs.st_uid);
  fs.gid   = htons(m->fs.st_gid);
  fs.sizel = htonl((uint32_t)(m->fs.st_size & 0xFFFFFFFF));
  fs.sizeh = htonl((uint32_t)(m->fs.st_size >> 32));
  fs.mtime = htonl(m->fs.st_mtime);
  fs.ctime = htonl(m->fs.st_ctime);
  fs.fnlen = htons(strlen(m->fn));
  fs.lnlen = htons(strlen(m->ln));
  dev = (uint64_t)(m->fs.st_rdev);

  m->mdata.head.szdata = 0;
  data_safeset(&(m->mdata), &fs, sizeof(fs));
  data_safeset(&(m->mdata), m->fn, strlen(m->fn));
  data_safeset(&(m->mdata), m->ln, strlen(m->ln));
  data_safeset32(&(m->mdata), (uint32_t)(dev >> 32));
  data_safeset32(&(m->mdata), (uint32_t)(dev & 0xFFFFFFFF));
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, -1);
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_send_stat_delete_report(mfile *m)
{
  mhost   *t;
  uint8_t *r;
  char *dryrun = "";

  if(m->dryrun){
    dryrun = "(dryrun) ";
    if(ack_check(m, MAKUO_RECVSTATE_DELETEOK) == 1){
      if(m->comm){
        if(m->comm->loglevel == 0){
          cprintf(0, m->comm, "%s[delete:%s]\n", dryrun, m->fn);
        }
      }
    }
  }

  for(t=members;t;t=t->next){
    if(m->sendto){
      if(t != member_get(&(m->addr.sin_addr))){
        continue;
      }
    }
    if((r = get_hoststate(t, m))){
      if(*r == MAKUO_RECVSTATE_DELETEOK){
        cprintf(1, m->comm, "%sdelete %s:%s\n", dryrun, t->hostname, m->fn);
        lprintf(1, "%sdelete %s:%s\n", dryrun, t->hostname, m->fn);
      }
    }
  }
}

static void msend_req_send_stat_update_report(mfile *m)
{
  uint8_t *r;
  mhost   *t;
  char *dryrun = "";

  if(m->dryrun){
    dryrun = "(dryrun) ";
    if(ack_check(m, MAKUO_RECVSTATE_UPDATE) == 1){
      if(m->comm){
        if(m->comm->loglevel == 0){
          cprintf(0, m->comm, "%s[update:%s]\n", dryrun, m->fn);
        }
      }
    }
  }

  for(t=members;t;t=t->next){
    if(m->sendto){
      if(t != member_get(&(m->addr.sin_addr))){
        continue;
      }
    }
    if((r = get_hoststate(t, m))){
      if(*r == MAKUO_RECVSTATE_UPDATE){
        cprintf(1, m->comm, "%supdate %s:%s\r\n", dryrun, t->hostname, m->fn);
        lprintf(1, "%supdate %s:%s\n", dryrun, t->hostname, m->fn);
      }
      if(*r == MAKUO_RECVSTATE_SKIP){
        cprintf(2, m->comm, "%sskip   %s:%s\r\n", dryrun, t->hostname, m->fn);
        lprintf(5, "%sskip   %s:%s\n", dryrun, t->hostname, m->fn);
      }
      if(*r == MAKUO_RECVSTATE_READONLY){
        cprintf(3, m->comm, "%sskipro %s:%s\r\n", dryrun, t->hostname, m->fn);
        lprintf(6, "%sskipro %s:%s\n", dryrun, t->hostname, m->fn);
      }
    }
  }
}

static void msend_req_send_stat(int s, mfile *m)
{
  if(m->initstate){
    msend_req_send_stat_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->mdata.head.flags & MAKUO_FLAG_SYNC){
    msend_req_send_stat_delete_report(m);
    m->initstate = 1;
    m->mdata.head.ostate = m->mdata.head.nstate;
    m->mdata.head.nstate = MAKUO_SENDSTATE_LAST;
  }else{
    msend_req_send_stat_update_report(m);
    m->initstate = 1;
    m->mdata.head.ostate = m->mdata.head.nstate;
    if(m->dryrun){
      m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
    }else{
      if(ack_check(m, MAKUO_RECVSTATE_UPDATE) == 1){
        m->mdata.head.nstate = MAKUO_SENDSTATE_OPEN;
      }else{
        m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
      }
    }
  }
}

static void msend_req_send_open_init(int s, mfile *m)
{
  int e;
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, MAKUO_RECVSTATE_UPDATE);

  /*----- symlink -----*/
  if(S_ISLNK(m->fs.st_mode) || !S_ISREG(m->fs.st_mode)){
    msend_packet(s, &(m->mdata), &(m->addr));
  }else{
    m->fd = open(m->fn, O_RDONLY, 0);
    if(m->fd != -1){
      msend_packet(s, &(m->mdata), &(m->addr));
    }else{
      e = errno;
      m->sendwait  = 0;
      m->initstate = 1;
      m->mdata.head.ostate = m->mdata.head.nstate;
      m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
      cprintf(0, m->comm, "error: %s %s\n", strerror(e), m->fn);
      lprintf(0, "[error] %s: %s %s\n", __func__,   strerror(e), m->fn);
    }
  }
}

static void msend_req_send_open(int s, mfile *m)
{
  if(m->initstate){
    msend_req_send_open_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_UPDATE) == 1){
    m->sendwait = 1;
    ack_clear(m, MAKUO_RECVSTATE_UPDATE);
    return;
  }
  if(S_ISLNK(m->fs.st_mode) || !S_ISREG(m->fs.st_mode)){
    m->initstate = 1;
    m->mdata.head.ostate = m->mdata.head.nstate;
    m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
  }else{
    m->mdata.head.seqno  = 0;
    m->mdata.head.ostate = m->mdata.head.nstate;
    m->mdata.head.nstate = MAKUO_SENDSTATE_DATA;
  }
}

static void msend_req_send_markdata(int s, mfile *m)
{
  int   r;
  off_t offset;
  if(!m->mark){
    /* close */
    m->initstate = 1;
    m->mdata.head.ostate = m->mdata.head.nstate;
    m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
    return;
  }
  m->mdata.head.seqno = seq_getmark(m);
  offset = m->mdata.head.seqno;
  offset *= MAKUO_BUFFER_SIZE;
  lseek(m->fd, offset, SEEK_SET);
  r = read(m->fd, m->mdata.data, MAKUO_BUFFER_SIZE);
  if(r>0){
    m->mdata.head.szdata = r;
    msend_packet(s, &(m->mdata), &(m->addr));
    if(!m->mark){
      m->initstate = 1;
      m->mdata.head.nstate = MAKUO_SENDSTATE_MARK;
    }
    return;
  }
  if(!r){
    lprintf(0, "[error] %s: read eof? seqno=%d %s\n", __func__, m->mdata.head.seqno, m->fn);
    cprintf(0, m->comm, "error: read eof? seqno=%d %s\n", m->mdata.head.seqno, m->fn);
  }else{
    lprintf(0, "[error] %s: can't read (%s) seqno=%d %s\n",   __func__, strerror(errno), m->mdata.head.seqno, m->fn);
    cprintf(0, m->comm, "error: can't read (%s) seqno=%d %s\n", strerror(errno), m->mdata.head.seqno, m->fn);
  }
  m->mdata.head.nstate = MAKUO_SENDSTATE_BREAK;
  m->initstate = 1;
}

static void msend_req_send_filedata(int s, mfile *m)
{
  off_t offset;
  int readsize;
  if(m->mark){
    m->mdata.head.seqno = seq_getmark(m);
  }else{
    m->mdata.head.seqno = m->seqnonow++;
  }
  offset  = m->mdata.head.seqno;
  offset *= MAKUO_BUFFER_SIZE;
  lseek(m->fd, offset, SEEK_SET);
  readsize = read(m->fd, m->mdata.data, MAKUO_BUFFER_SIZE);
  if(readsize > 0){
    m->mdata.head.szdata = readsize;
    msend_packet(s, &(m->mdata), &(m->addr));
  }else{
    if(readsize == -1){
      /* err */
      lprintf(0, "[error] %s: can't read (%s) seqno=%d %s\n",   __func__, strerror(errno), m->mdata.head.seqno, m->fn);
      cprintf(0, m->comm, "error: can't read (%s) seqno=%d %s\n", strerror(errno), m->mdata.head.seqno, m->fn);
      m->mdata.head.nstate = MAKUO_SENDSTATE_BREAK;
      m->initstate = 1;
    }else{
      /* eof */
      lprintf(9, "%s: block send count=%d %s\n", __func__, m->mdata.head.seqno, m->fn);
      m->mdata.head.seqno  = 0;
      m->mdata.head.nstate = MAKUO_SENDSTATE_MARK;
      m->initstate = 1;
      m->lickflag  = 1;
    }
  }
}

static void msend_req_send_data(int s, mfile *m)
{
  if(m->lickflag){
    msend_req_send_markdata(s, m); /* send retry */
  }else{
    msend_req_send_filedata(s, m); /* send data  */
  }
}

static void msend_req_send_mark_init(int s, mfile *m)
{
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, MAKUO_RECVSTATE_UPDATE);
  ack_clear(m, MAKUO_RECVSTATE_OPEN);
  ack_clear(m, MAKUO_RECVSTATE_MARK);
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_send_mark(int s, mfile *m)
{
  if(m->initstate){
    msend_req_send_mark_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_UPDATE) == 1){
    msend_req_send_mark_init(s, m);
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_OPEN) == 1){
    msend_req_send_mark_init(s, m);
    return;
  }
  m->mdata.head.nstate = MAKUO_SENDSTATE_DATA;
}

static void msend_req_send_close_init(int s, mfile *m)
{
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, MAKUO_RECVSTATE_UPDATE);
  ack_clear(m, MAKUO_RECVSTATE_OPEN);
  ack_clear(m, MAKUO_RECVSTATE_MARK);
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_send_close(int s, mfile *m)
{
  if(m->initstate){
    msend_req_send_close_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_UPDATE) == 1){
    msend_req_send_close_init(s, m);
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_OPEN) == 1){
    msend_req_send_close_init(s, m);
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_MARK) == 1){
    msend_req_send_close_init(s, m);
    return;
  }
  if(m->mdata.head.ostate == MAKUO_SENDSTATE_MARK || 
     m->mdata.head.ostate == MAKUO_SENDSTATE_DATA ||
     m->mdata.head.ostate == MAKUO_SENDSTATE_OPEN){
    lprintf(4, "update complate %s\n", m->fn);
  }
  m->initstate = 1;
  m->mdata.head.nstate = MAKUO_SENDSTATE_LAST;
}

static void msend_req_send_last_init(int s, mfile *m)
{
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, -1);
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_send_last(int s, mfile *m)
{
  if(m->initstate){
    msend_req_send_last_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_SKIP) == 1){
    msend_req_send_last_init(s, m);
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_CLOSE) == 1){
    msend_req_send_last_init(s, m);
    return;
  }
  msend_mfdel(m);
}

/*----- send -----*/
static void msend_req_send(int s, mfile *m)
{
  if(!m->comm){
    if(m->mdata.head.nstate != MAKUO_SENDSTATE_BREAK){
      m->initstate = 1;
      m->mdata.head.nstate = MAKUO_SENDSTATE_BREAK;
    }
  }
  switch(m->mdata.head.nstate){
    case MAKUO_SENDSTATE_STAT:
      msend_req_send_stat(s, m);
      break;
    case MAKUO_SENDSTATE_OPEN:
      msend_req_send_open(s, m);
      break;
    case MAKUO_SENDSTATE_DATA:
      msend_req_send_data(s, m);
      break;
    case MAKUO_SENDSTATE_MARK:
      msend_req_send_mark(s, m);
      break;
    case MAKUO_SENDSTATE_CLOSE:
      msend_req_send_close(s, m);
      break;
    case MAKUO_SENDSTATE_LAST:
      msend_req_send_last(s, m);
      break;
    case MAKUO_SENDSTATE_BREAK:
      msend_req_send_break(s, m);
      break;
  }
}

static void msend_req_md5_open_init(int s, mfile *m)
{
  int e;
  int r;
  mhash *h;
  char buff[8192];

  h = (mhash *)m->mdata.data;
  r = read(m->fd, buff, sizeof(buff));
  if(r > 0){
    MD5_Update(&(m->md5), buff, r);
    return;
  }
  e = errno;
  if(r == -1){
    if(e == EINTR){
      return;
    }
	  lprintf(0, "[error] %s: file read error(%s) %s\n", __func__, strerror(e), m->fn);
    cprintf(0, m->comm, "error: file read error(%s) %s\n", strerror(e), m->fn);
    MD5_Final(h->hash, &(m->md5));
    close(m->fd);
    m->fd = -1;
    msend_mfdel(m);
    return;
  }
  MD5_Final(h->hash, &(m->md5));
  close(m->fd);
  m->fd = -1;
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, -1);
  m->mdata.head.nstate = MAKUO_SENDSTATE_OPEN;
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_md5_open(int s, mfile *m)
{
  if(m->initstate){
    msend_req_md5_open_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_OPEN) == 1){
    m->initstate = 0;
    m->sendwait  = 1;
    ack_clear(m, MAKUO_RECVSTATE_OPEN);
    return;
  }
  m->initstate = 1;
  m->sendwait  = 0;
  m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
}

static void msend_req_md5_close_init(int s, mfile *m)
{
  m->sendwait  = 1;
  m->initstate = 0;
  ack_clear(m, MAKUO_RECVSTATE_MD5OK);
  ack_clear(m, MAKUO_RECVSTATE_MD5NG);
  m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
  msend_packet(s, &(m->mdata), &(m->addr));
}

static void msend_req_md5_close(int s, mfile *m)
{
  if(m->initstate){
    msend_req_md5_close_init(s, m);
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  msend_mfdel(m);
}

/*----- md5 -----*/
static void msend_req_md5(int s, mfile *m)
{
  if(!m->comm){
    if(m->mdata.head.nstate == MAKUO_SENDSTATE_OPEN){
      if(m->initstate){
        MD5_Final(m->mdata.data, &(m->md5));
        close(m->fd);
        m->fd = -1;
        msend_mfdel(m);
        return;
      }else{
        m->initstate = 1;
        m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
      }
    }
  }
  switch(m->mdata.head.nstate){
    case MAKUO_SENDSTATE_OPEN:
      msend_req_md5_open(s, m);
      break;
    case MAKUO_SENDSTATE_CLOSE:
      msend_req_md5_close(s, m);
      break;
  }
}

static void msend_req_dsync_open(int s, mfile *m)
{
  if(m->initstate){
    m->initstate = 0;
    m->sendwait  = 1;
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  m->initstate = 1;
  m->mdata.head.nstate = MAKUO_SENDSTATE_DATA;
}

static void msend_req_dsync_data_init(int s, mfile *m)
{
  uint16_t len;
  int excludecnt = 0;
  excludeitem *e = NULL;

  m->sendwait  = 1;
  m->initstate = 0;
  for(e=m->comm->exclude;e;e=e->next){
    if(excludecnt == m->mdata.head.seqno){
      break;
    }
    excludecnt++;
  }
  m->mdata.head.szdata = 0;
  while(e){
    len = strlen(e->pattern);
    if(m->mdata.head.szdata + sizeof(uint16_t) + len > MAKUO_BUFFER_SIZE){
      break;
    }
    data_safeset16(&(m->mdata), len);
    data_safeset(&(m->mdata), e->pattern, len);
    m->mdata.head.seqno++;
    e = e->next;
  }
  if(m->mdata.head.szdata == 0){
    m->mdata.head.seqno++;
  }
}

static void msend_req_dsync_data(int s, mfile *m)
{
  if(m->initstate){
    msend_req_dsync_data_init(s, m);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  m->initstate = 1;
  if(m->mdata.head.szdata == 0){
    m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
  }
}

static void msend_req_dsync_close(int s, mfile *m)
{
  if(m->initstate){
    m->sendwait  = 1;
    m->initstate = 0;
    ack_clear(m, MAKUO_RECVSTATE_OPEN);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(ack_check(m, MAKUO_RECVSTATE_OPEN)){
    m->sendwait  = 0;
    m->initstate = 1;
  }else{
    msend_packet(s, &(m->mdata), &(m->addr));
    msend_mfdel(m);
  }
}

static void msend_req_dsync_break(int s, mfile *m)
{
  if(m->initstate){
    m->initstate = 0;
    m->sendwait  = 1;
    ack_clear(m, -1);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  msend_mfdel(m);
}

/*----- dsync -----*/
static void msend_req_dsync(int s, mfile *m)
{
  if(!m->comm){
    if(m->mdata.head.nstate != MAKUO_SENDSTATE_BREAK){
      m->initstate = 1;
      m->mdata.head.nstate = MAKUO_SENDSTATE_BREAK;
    }
  }
  switch(m->mdata.head.nstate){
    case MAKUO_SENDSTATE_OPEN:
      msend_req_dsync_open(s, m);
      break;
    case MAKUO_SENDSTATE_DATA:
      msend_req_dsync_data(s, m);
      break;
    case MAKUO_SENDSTATE_CLOSE:
      msend_req_dsync_close(s, m);
      break;
    case MAKUO_SENDSTATE_BREAK:
      msend_req_dsync_break(s, m);
      break;
  }
}

static int msend_req_del_stat_read_pathcmp(int s, mfile *m)
{
  char *p1;
  char *p2;
  mfile *a;
  for(a=mftop[MFRECV];a;a=a->next){
    if(a->mdata.head.opcode == MAKUO_OP_SEND){
      p1 = a->tn;
      p2 = m->tn;
      while((strlen(p1) > 1) && (memcmp(p1, "./" ,2) == 0)){
        p1 += 2;
      }
      while((strlen(p2) > 1) && (memcmp(p2, "./" ,2) == 0)){
        p2 += 2;
      }
      if(!strcmp(p1,p2)){
        return(1);
      }
    }
  }
  return(0);
}

static void msend_req_del_stat_read(int s, mfile *m)
{
  int r;
  mfile *d;
  uint16_t len;

  for(d=mftop[MFSEND];d;d=d->next){
    if(d->link == m){
      if(d->mdata.head.nstate == MAKUO_SENDSTATE_WAIT){
        break;
      }
    }
  }
  if(!d){
    d = mkreq(&(m->mdata), &(m->addr), MAKUO_SENDSTATE_WAIT);
    d->mdata.head.flags = m->mdata.head.flags;
    d->mdata.head.reqid = getrid();
    d->initstate = 1;
    d->sendwait  = 0;
    d->sendto    = 1;
    d->dryrun    = m->dryrun;
    d->recurs    = m->recurs;
    d->link      = m;
    d->mdata.p   = d->mdata.data;
  }

  if(m->len >= sizeof(m->mod)){
    len = m->len - sizeof(m->mod);
    data_safeset16(&(d->mdata), m->len);
    data_safeset32(&(d->mdata), m->mod);
    data_safeset(&(d->mdata), m->tn, len);
    m->len = 0;
  }

  while(1){
    if((r = atomic_read(m->pipe, &(m->len), sizeof(m->len), 1))){
      if(r == -1){
        if(errno == EAGAIN){
          return;
        }else{
          lprintf(0, "[error] %s: length read error\n", __func__);
        }
      }
      break;
    }
    if(m->len <= sizeof(m->mod)){
      lprintf(0, "[error] %s: length error\n", __func__);
      break;
    }
    len = m->len - sizeof(m->mod);
    if(atomic_read(m->pipe, &(m->mod), sizeof(m->mod), 0)){
      lprintf(0, "[error] %s: filemode read error\n", __func__);
      break;
    }
    if(atomic_read(m->pipe, m->tn, len, 0)){
      lprintf(0, "[error] %s: filename read error\n", __func__);
      break;
    }
    m->tn[len] = 0;
    if(lstat(m->tn, &(m->fs)) == -1){
      if(errno == ENOENT){
        m->len = 0;
        continue;
      }
    }
    if(msend_req_del_stat_read_pathcmp(s, m)){
      m->len = 0;
      continue;
    }
    if(d->mdata.head.szdata + sizeof(m->len) + m->len > MAKUO_BUFFER_SIZE){
      d->mdata.head.nstate = MAKUO_SENDSTATE_OPEN;
    }else{
      strcpy(d->fn, m->tn);
      data_safeset16(&(d->mdata), m->len);
      data_safeset32(&(d->mdata), m->mod);
      data_safeset(&(d->mdata), m->tn, len);
      m->len = 0;
    }
    return;
  }
  d->mdata.head.nstate = MAKUO_SENDSTATE_OPEN;
  close(m->pipe);
  m->pipe      = -1;
  m->initstate =  1;
  m->sendwait  =  0;
}

static int msend_req_del_stat_waitcheck(int s, mfile *m)
{
  mfile *a;
  for(a=mftop[0];a;a=a->next){
    if((a->mdata.head.opcode == MAKUO_OP_DEL) && (a->link == m)){
      return(1);
    }
  }
  return(0);
}

static void msend_req_del_stat(int s, mfile *m)
{
  if(m->pipe != -1){
    msend_req_del_stat_read(s, m);
    return;
  }
  if(msend_req_del_stat_waitcheck(s, m)){
    m->sendwait = 1;
    return;
  }
  if(m->link){
    msend(mkack(&(m->link->mdata), &(m->link->addr), MAKUO_RECVSTATE_CLOSE)); /* send ack for dsync */
  }
  if(waitpid(m->pid, NULL, WNOHANG) != m->pid){
    m->sendwait = 1;
  }else{
    m->pid = 0;
    msend_mfdel(m);
  }
}

static void msend_req_del_break(int s, mfile *m)
{
  mfile *d;
  for(d=mftop[MFSEND];d;d=d->next){
    if(d->link == m){
      if(d->mdata.head.nstate == MAKUO_SENDSTATE_WAIT){
        d->link = NULL;
        msend_mfdel(d);
        break;
      }
    }
  }
  msend_mfdel(m);
  lprintf(0,"%s: break dsync\n", __func__);
}

static void msend_req_del_open(int s, mfile *m)
{
  if(m->initstate){
    m->initstate = 0;
    m->sendwait  = 1;
    ack_clear(m, -1);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  m->initstate = 1;
}

static void msend_req_del_data(int s, mfile *m)
{
  if(m->initstate){
    m->initstate = 0;
    m->sendwait  = 1;
    ack_clear(m, -1);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  m->initstate = 1;
  m->sendwait  = 0;
  m->mdata.head.nstate = MAKUO_SENDSTATE_CLOSE;
}

static void msend_req_del_close(int s, mfile *m)
{
  lprintf(0, "%s: 0\n", __func__);
  if(m->initstate){
    lprintf(1, "%s: 0\n", __func__);
    m->initstate = 0;
    m->sendwait  = 1;
    ack_clear(m, -1);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->sendwait){
    lprintf(2, "%s: 0\n", __func__);
    msend_packet(s, &(m->mdata), &(m->addr));
    return;
  }
  if(m->link){
    lprintf(3, "%s: 0\n", __func__);
    m->link->sendwait = 0;
  }
  m->link = NULL;
  msend_mfdel(m);
  lprintf(4, "%s: 0\n", __func__);
}

/*----- del -----*/
static void msend_req_del(int s, mfile *m)
{
  switch(m->mdata.head.nstate){
    case MAKUO_SENDSTATE_STAT:
      msend_req_del_stat(s, m);
      break;
    case MAKUO_SENDSTATE_BREAK:
      msend_req_del_break(s, m);
      break;
    case MAKUO_SENDSTATE_OPEN:
      msend_req_del_open(s, m);
      break;
    case MAKUO_SENDSTATE_DATA:
      msend_req_del_data(s, m);
      break;
    case MAKUO_SENDSTATE_CLOSE:
      msend_req_del_close(s, m);
      break;
  }
}

/*----- exit -----*/
static void msend_req_exit(int s, mfile *m)
{
  msend_shot(s, m);
}

/*----- ping -----*/
static void msend_req_ping(int s, mfile *m)
{
  msend_shot(s, m);
}

/*----- send request -----*/
static void msend_req(int s, mfile *m)
{
  switch(m->mdata.head.opcode){
    case MAKUO_OP_PING:
      msend_req_ping(s, m);
      break;
    case MAKUO_OP_EXIT:
      msend_req_exit(s, m);
      break;
    case MAKUO_OP_SEND:
      msend_req_send(s, m);
      break;
    case MAKUO_OP_MD5:
      msend_req_md5(s, m);
      break;
    case MAKUO_OP_DSYNC:
      msend_req_dsync(s, m);
      break;
    case MAKUO_OP_DEL:
      msend_req_del(s, m);
      break;
    /* 機能追加はここ */
  }
}

/******************************************************************
*
* send common functions (public)
*
*******************************************************************/
void msend(mfile *m)
{
  if(!m){
    return;
  }
  if(m->mdata.head.flags & MAKUO_FLAG_ACK){
    msend_ack(moption.mcsocket, m); 
  }else{
    if(!msend_retry(m)){
      msend_req(moption.mcsocket, m); 
      mtimeget(&m->lastsend);
    }
  }
}

void msend_clean()
{
  mfile *m = mftop[MFSEND];
  while((m = msend_mfdel(m)));
}

