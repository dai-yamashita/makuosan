/*
 * makuosan.c
 * Copyright (C) 2008-2012 KLab Inc.
 */
#include "makuosan.h"

void recv_timeout(mfile *m)
{
  mhost   *t;
  uint8_t *r;
  if(!m){
    return;
  }
  do{
    for(t=members;t;t=t->next){
      r = get_hoststate(t, m);
      if(*r == MAKUO_RECVSTATE_NONE){
        member_del_message(1, t, "receive time out");
        member_del(t);
        break;
      }
    }
  }while(t); 
  m->retrycnt = MAKUO_SEND_RETRYCNT;
}

void pingpong(int n)
{
  int i;
  unsigned int s;
  struct timeval tv;
  mfile *m = mfins(MFSEND);
  mping *p = NULL;
  char buff[MAKUO_HOSTNAME_MAX + 1];

  if(!m){
    lprintf(0, "[error] %s: out of memmory\r\n", __func__);
    return;
  }
  m->mdata.head.reqid  = getrid();
  m->mdata.head.seqno  = 0;
  m->mdata.head.szdata = 0;
  m->sendwait          = 0;
  if(gethostname(buff, sizeof(buff)) == -1){
    buff[0] = 0;
  }
  p = (mping *)(m->mdata.data);
  p->hostnamelen = strlen(buff);
  p->versionlen  = strlen(PACKAGE_VERSION);
  m->mdata.head.szdata = sizeof(mping) + p->hostnamelen + p->versionlen;
  m->mdata.p = p->data;
  memcpy(m->mdata.p, buff, p->hostnamelen);
  m->mdata.p += p->hostnamelen;
  memcpy(m->mdata.p, PACKAGE_VERSION, p->versionlen);
  m->mdata.p += p->versionlen;
  p->hostnamelen = htons(p->hostnamelen);
  p->versionlen  = htons(p->versionlen);
  gettimeofday(&lastpong, NULL);
  switch(n){
    case 0:
      for(i=0;buff[i];i++){
        s += buff[i];
      }
      gettimeofday(&tv,NULL);
      s += tv.tv_usec;
      srand(s);
      m->mdata.head.opcode = MAKUO_OP_PING;
      break;
    case 1:
      m->mdata.head.opcode = MAKUO_OP_PING;
      m->mdata.head.flags |= MAKUO_FLAG_ACK;
      break;
    case 2:
      m->mdata.head.opcode = MAKUO_OP_EXIT;
      msend(m);
      break;
  } 
}

int mfdirchk(mfile *d){
  mfile *m;
  int len = strlen(d->fn);
  if(!len){
    return(1);
  }
  if(d->fn[len - 1] == '/'){
    len--;
  }
  if(d->mdata.head.flags & MAKUO_FLAG_ACK){
    return(1);
  }
  for(m=mftop[MFSEND];m;m=m->next){
    if(m == d){
      continue;
    }
    if(m->comm != d->comm){
      continue;
    }
    if(strlen(m->fn) < len){
      continue;
    }
    if(!memcmp(d->fn, m->fn, len)){
      if(m->fn[len] == '/'){
        return(0);
      }
    }
  }
  return(1);
}

int is_send(mfile *m)
{
  if(!m){
    return(0);
  }
  if(m->mdata.head.nstate == MAKUO_SENDSTATE_WAIT){
    return(0);
  }
  if(m->mdata.head.flags & MAKUO_FLAG_ACK){
    return(1);
  }
  switch(m->mdata.head.opcode){
    case MAKUO_OP_SEND:
    case MAKUO_OP_DEL:
      if(!S_ISLNK(m->fs.st_mode) && S_ISDIR(m->fs.st_mode)){
        if(!mfdirchk(m)){
          return(0);
        }
      }
      break;
  }
  if(!ack_check(m, MAKUO_RECVSTATE_NONE)){
    m->sendwait = 0;
  }
  if(m->sendwait){
    if(!mtimeout(&(m->lastsend), MAKUO_SEND_TIMEOUT)){
      return(0);
    }
    if(!(m->retrycnt)){
      recv_timeout(m);
    }
  }
  return(1);
}

void rfdset(int s, fd_set *fds)
{
  FD_SET(s, fds);
}

void wfdset(int s, fd_set *fds)
{
  mfile *m;
  for(m=mftop[MFSEND];m;m=m->next){
    if(is_send(m)){
      FD_SET(s, fds);
      return;
    }
  }
}

void cfdset(mcomm *c, fd_set *rfds, fd_set *wfds)
{
  int i;

  /*----- listen socket -----*/
  if(moption.lisocket != -1){
    FD_SET(moption.lisocket, rfds);
  }

  /*----- connect socket -----*/
  for(i=0;i<MAX_COMM;i++){
    if(c[i].fd[0] != -1){
      FD_SET(c[i].fd[0], rfds);
      if(c[i].working){
        FD_SET(c[i].fd[0], wfds);
      }
    }
    if(c[i].fd[1] != -1){
      FD_SET(c[i].fd[1], rfds);
    }else{
      if(c[i].cpid){
        if(waitpid(c[i].cpid, NULL, WNOHANG) == c[i].cpid){
          c[i].cpid = 0;
        }
      }
    }
  }
}

int do_select(fd_set *rfds, fd_set *wfds)
{
  struct timeval tv;
  tv.tv_sec  = 1;
  tv.tv_usec = 0;
  if(select(1024, rfds, wfds, NULL, &tv) <= 0){
    gettimeofday(&curtime, NULL);
    moption.sendready = 0;
    return(-1);
  }
  gettimeofday(&curtime, NULL);
  moption.sendready = FD_ISSET(moption.mcsocket, wfds);
  return(0);
}

void do_pong()
{
  static uint32_t interval = MAKUO_PONG_INTERVAL;
  if(mtimeout(&lastpong, interval)){
    interval  = MAKUO_PONG_INTERVAL;
    interval += ((rand() % 21) - 10) * 1000;
    pingpong(1);
  }
}

void do_free()
{
  mrecv_gc();
}

void do_recv()
{
  int c = 0;
  mhost  *t;
  for(t=members;t;t=t->next){
    c++;
  }
  while(mrecv()){
    if(c){
      c--;
    }else{
      break;
    }
  }
}

void do_send()
{
  int  i=0;
  mfile *m;
  mfile *n;

  for(m=mftop[MFSEND];m;m=n){
    if(i == moption.parallel){
      return;
    }
    n = m->next;
    if(m->mdata.head.flags & MAKUO_FLAG_ACK){
      msend(m);
      continue;
    }
    if(!is_send(m)){
      if(m->sendwait){
        i++;
      }
      continue;
    }
    msend(m);
    i++;
  }
}

int do_accept(mcomm *c, fd_set *fds)
{
  int i;
  int s = moption.lisocket;
  struct sockaddr_storage ss;
  socklen_t sslen = sizeof(ss);
  char buff[1024];

  if(s == -1){
    return(0);
  }
  if(!FD_ISSET(s,fds)){
    return(0);
  }
  for(i=0;i<MAX_COMM;i++){
    if(c[i].fd[0] == -1){
      break;
    }
  }
  if(i==MAX_COMM){
    close(accept(s, NULL, 0)); 
    lprintf(0, "[error] %s: can't accept reached in the maximum\n");
    return(1);
  }
  c[i].fd[0] = accept(s, (struct sockaddr *)&ss, &sslen);
  switch(ss.ss_family){
    case AF_UNIX:
      lprintf(5, "%s: socket=%d from %s\n", __func__, i, moption.uaddr.sun_path);
      break;
    case AF_INET:
    case AF_INET6:
      buff[0]=0;
      getnameinfo((const struct sockaddr *)&ss, sizeof(ss), buff, sizeof(buff), NULL, 0, NI_NUMERICHOST);
      lprintf(5, "%s: socket=%d from %s\n", __func__, i, buff);
      break;
  }
  c[i].working = 1;
  if(fcntl(c[i].fd[0], F_SETFL , O_NONBLOCK)){
    lprintf(0, "[error] %s: fcntl: %s\n", __func__, strerror(errno));
  }
  return(0);
}

int do_comexe(mcomm *c, fd_set *fds){
  int i, j;
  for(i=0;i<MAX_COMM;i++){
    for(j=0;j<2;j++){
      if(c[i].fd[j] != -1){
        if(FD_ISSET(c[i].fd[j], fds) || c[i].check[j]){
          mexec(&c[i], j);
        }
      }
    }
  }
  return(0);
}

void do_exechk(mcomm *c){
  int    i;
  mfile *m;
  for(i=0;i<MAX_COMM;i++){
    if(c[i].working){
      if(c[i].isalive){
        if(mtimeout(&(c[i].tv), (uint32_t)1000)){
          cprintf(0, &(c[i]), "alive\n");
          mtimeget(&(c[i].tv));
        }
      }
      if((c[i].cpid == 0) && (c[i].fd[1] == -1)){
        for(m=mftop[MFSEND];m;m=m->next){
          if(m->comm == &c[i]){
            break; /* working */
          }
        }
        if(!m){
          workend(&c[i]);
        }
      }
    }
  }
}

void mloop()
{
  fd_set rfds;
  fd_set wfds;
  while(loop_flag){
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    rfdset(moption.mcsocket, &rfds);
    wfdset(moption.mcsocket, &wfds);
    cfdset(moption.comm, &rfds, &wfds);
    if(do_select(&rfds, &wfds)){
      do_pong();
      do_free();
    }else{
      do_pong();
      do_recv();
      do_send();
      do_accept(moption.comm, &rfds);
      do_comexe(moption.comm, &rfds);
      do_exechk(moption.comm);
    }
    if(log_level != moption.loglevel){
      lprintf(0, "%s: loglevel change %d to %d\n", __func__, moption.loglevel, log_level);
      moption.loglevel = log_level;
    }
  }
}

int main(int argc, char *argv[])
{
  minit(argc,argv);
  pingpong(0);
  mloop();
  pingpong(2);
  mexit();
  return(0);
}

