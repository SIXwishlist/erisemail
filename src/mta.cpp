/*
	Copyright (c) openheap, uplusware
	uplusware@gmail.com
*/
#include <stdio.h>
#include <errno.h>
#include "util/general.h"
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#include "mta.h"
#include "letter.h"
#include "util/trace.h"
#include "storage.h"
#include "dns.h"
#include "smtpclient.h"
#include "letter.h"
#include "pool.h"
#include "service.h"

typedef struct
{
	Mail_Info mail_info;
	StorageEngine* _storageEngine;
	memcached_st * _memcached;
}ReplyInfo;


static volatile BOOL s_relay_stop = FALSE;
static volatile unsigned long s_relay_count = 0;

static std::queue<ReplyInfo*> gs_thread_pool_arg_queue;
static pthread_mutex_t gs_thread_pool_mutex;
static sem_t gs_thread_pool_sem;
static volatile unsigned int gs_thread_pool_size = 0;

static BOOL ReturnMail(MailStorage* mailStg, memcached_st * memcached,
    int mid, const char* uniqid, const char *mail_from, const char *rcpt_to, const char * errormsg)
{	
	int nBoundary = 0;
	char szBoundary[64];
	srandom(time(NULL));
	sprintf(szBoundary, "%03d_%04X_%08X.%08X", nBoundary, random()%0xFFFF, time(NULL));
	char szMailFrom[64];
	sprintf(szMailFrom, "postmaster@%s", CMailBase::m_email_domain.c_str());

	string strRcptTo = mail_from;

	char szUid[1024];
	sprintf(szUid, "%08x_%08x_%016lx_%08x_%s", time(NULL), getpid(), pthread_self(), random(), CMailBase::m_localhostname.c_str());

	string strtmp = "";

	string rcpttoid, rcpttodomain;

	strcut(strRcptTo.c_str(), NULL, "@", rcpttoid);
	strcut(strRcptTo.c_str(), "@", NULL, rcpttodomain);

	int inBoxID = -1;
		
	if( mailStg->GetInboxID(rcpttoid.c_str(), inBoxID) == -1)
		return FALSE;

	MailType mType;
	string strMailTo;
	unsigned long long usermaxsize;
	
	if(CMailBase::Is_Local_Domain(rcpttodomain.c_str()))
	{
		if(mailStg->GetUserSize(rcpttoid.c_str(), usermaxsize) == -1)
		{
			usermaxsize = 5000*1024;
		}

		mType = mtLocal;
		strMailTo = "";
	}
	else
	{
		
		if(mailStg->GetUserSize(szMailFrom, usermaxsize) == -1)
		{
			usermaxsize = 5000*1024;
		}
		mType = mtExtern;
		inBoxID = -1;
		strMailTo = strRcptTo.c_str();		
	}
	
	MailLetter* newLetter  = new MailLetter(mailStg, CMailBase::m_private_path.c_str(), CMailBase::m_encoding.c_str(), memcached, szUid, usermaxsize);
	
    if(!newLetter)
        return FALSE;
    
	Letter_Info letter_info;
	letter_info.mail_from = szMailFrom;
	letter_info.mail_to = strMailTo.c_str();
	letter_info.mail_type = mType;
	letter_info.mail_uniqueid = szUid;
	letter_info.mail_dirid = inBoxID;
	letter_info.mail_status = 0;
	letter_info.mail_time = time(NULL);
	letter_info.user_maxsize = usermaxsize;
	letter_info.mail_id = -1;
				
	string emlfile;
	mailStg->GetMailIndex(mid, emlfile);

	char sztmp[1024];

	sprintf(sztmp, "Message-ID: <%08x_%08x_%016lx_%08x@%s>\r\n", time(NULL), getpid(), pthread_self(), random(), CMailBase::m_email_domain.c_str());
	
	strtmp += sztmp;
	
	sprintf(sztmp, "From: \"postmaster\" <postmaster@%s>\r\n", CMailBase::m_email_domain.c_str());	
	strtmp += sztmp;
	
	sprintf(sztmp, "To: \"%s@%s\"\r\nSubject: System Return Mail\r\n", rcpttoid.c_str(), rcpttodomain.c_str());
	strtmp += sztmp;

	string strTime;
	OutTimeString(time(NULL), strTime);

	strtmp += "Date: ";
	strtmp += strTime;
	strtmp += "\r\n";
	strtmp += "MIME-Version: 1.0\r\n"
				"Content-Type: multipart/mixed;\r\n"
				"	boundary=\"----=_NextPart_";

	strtmp += szBoundary;
	strtmp += "\"\r\n\r\n"
				"This is a multi-part message in MIME format.\r\n"
				"------=_NextPart_";
	strtmp += szBoundary;

	strtmp += "\r\n"
			"Content-Type: text/plain;";
			
	strtmp +=" charset=\"" + CMailBase::m_encoding + "\";\r\n";
	
	strtmp += "Content-Transfer-Encoding: 7bit\r\n\r\n"
			"This message is generated by e-risemail system.\r\n"
			"Sorry to have to inform you that the message returned.\r\n"
			"\r\n";
			
	sprintf(sztmp, "Send to \"%s\" failed\r\nCause: %s\r\n", rcpt_to, errormsg);
	strtmp += sztmp;

	strtmp +="------=_NextPart_";
	strtmp += szBoundary;
	strtmp += "\r\n"
			"Content-Type: message/rfc822;\r\n";

	sprintf(sztmp, " name=\"%s.eml\"\r\n", uniqid);
	strtmp += sztmp;
	
	strtmp +="Content-Transfer-Encoding: 7bit\r\nContent-Disposition: inline;\r\n";
	sprintf(sztmp, " filename=\"%s.eml\";\r\n\r\n", uniqid);
	strtmp += sztmp;

	newLetter->Write(strtmp.c_str(), strtmp.length());

	char codebuf[73];
	
	MailLetter* oldLetter = new MailLetter(mailStg, CMailBase::m_private_path.c_str(), CMailBase::m_encoding.c_str(), memcached, emlfile.c_str());
    if(oldLetter)
    {
        if(oldLetter->GetSize() > 0)
        {
            int llen;
            char* lbuf = oldLetter->Body(llen);

            int wlen = 0;
            while(1)
            {
                if(newLetter->Write(lbuf + wlen, (llen - wlen) > 1448 ? 1448 : (llen - wlen)) < 0)
                    break;
                wlen += ((llen - wlen) > 1448 ? 1448 : (llen - wlen));
                if(wlen >= llen)
                    break;
            }
        }
        oldLetter->Close();
        
        delete oldLetter;
    }
    
	strtmp = "\r\n";
	strtmp += "------=_NextPart_";
	strtmp += szBoundary;
	strtmp += "--\r\n";

	newLetter->Write(strtmp.c_str(), strtmp.length());
	newLetter->SetOK();
	newLetter->Close();

	mailStg->InsertMailIndex(letter_info.mail_from.c_str(), letter_info.mail_to.c_str(),letter_info.mail_time,
						letter_info.mail_type, letter_info.mail_uniqueid.c_str(), letter_info.mail_dirid, letter_info.mail_status,
						newLetter->GetEmlName(), newLetter->GetSize(), letter_info.mail_id);
                        

    delete newLetter;
    
	return TRUE;
}


static BOOL SendMail(MailStorage* mailStg, memcached_st * memcached, 
    const char* mxserver, const char* fromaddr, const char* toaddr,
    unsigned int mid, string& errormsg)
{
    /* Empty the error message */
    errormsg = "";
    /* Get the IP from the name */
	char realip[INET6_ADDRSTRLEN];
	struct addrinfo hints;      
    struct addrinfo *servinfo, *curr;  
    struct sockaddr_in *sa;
    struct sockaddr_in6 *sa6;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_CANONNAME; 
    
    if (getaddrinfo(mxserver, NULL, &hints, &servinfo) != 0)
    {
        errormsg = "can not get the corresponding mx server's ip\r\n";
        return FALSE;
    }
    
    BOOL bFound = FALSE;
    curr = servinfo; 
    while (curr && curr->ai_canonname)
    {  
        if(servinfo->ai_family == AF_INET6)
        {
            sa6 = (struct sockaddr_in6 *)curr->ai_addr;  
            inet_ntop(AF_INET6, (void*)&sa6->sin6_addr, realip, sizeof (realip));
            bFound = TRUE;
        }
        else if(servinfo->ai_family == AF_INET)
        {
            sa = (struct sockaddr_in *)curr->ai_addr;  
            inet_ntop(AF_INET, (void*)&sa->sin_addr, realip, sizeof (realip));
            bFound = TRUE;
        }
        curr = curr->ai_next;
    }     

    freeaddrinfo(servinfo);
    
    if(bFound == FALSE)
    {
        errormsg = "can not get the corresponding mx server's ip\r\n";
        return FALSE;
    }
    
    /* Connect to the MX server*/   
	int errorCode = 1;
	int errorCodeLen = sizeof(errorCode);
	int res;
	fd_set mask_r, mask_w; 
	struct timeval timeout; 
	int transfer_sockfd = -1;
	
	/* struct addrinfo hints; */
    struct addrinfo *server_addr, *rp;
    
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* Datagram socket */
    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
    hints.ai_protocol = 0;          /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
                
    int s = getaddrinfo((realip && realip[0] != '\0') ? realip : NULL, "25", &hints, &server_addr);
    if (s != 0)
    {
       fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
       return FALSE;
    }
    
    for (rp = server_addr; rp != NULL; rp = rp->ai_next)
    {
       transfer_sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
       if (transfer_sockfd == -1)
           continue;
       
	    int flags = fcntl(transfer_sockfd, F_GETFL, 0); 
	    fcntl(transfer_sockfd, F_SETFL, flags | O_NONBLOCK);
	
	    timeout.tv_sec = 10; 
	    timeout.tv_usec = 0;
	
        connect(transfer_sockfd, rp->ai_addr, rp->ai_addrlen);
        
        FD_ZERO(&mask_r);
	    FD_ZERO(&mask_w);
	
	    FD_SET(transfer_sockfd, &mask_r);
	    FD_SET(transfer_sockfd, &mask_w);
	    
	    if(select(transfer_sockfd + 1, &mask_r, &mask_w, NULL, &timeout) == 1)
	    {
	        break;  /* Success */
	    }
	    else
	    {
		    close(transfer_sockfd);
		    continue;
	    }  
           
       close(transfer_sockfd);
    }
     
    if (rp == NULL)
    {
        errormsg = "can not connect the ";
		errormsg += mxserver;
        errormsg += ".\r\n";
        /* No address succeeded */
        fprintf(stderr, "Could not connect\n");
        return FALSE;
    }

    freeaddrinfo(server_addr);           /* No longer needed */
        
    /* printf("[%s]: <%s>\n", mxserver, realip); */
	
	getsockopt(transfer_sockfd,SOL_SOCKET,SO_ERROR,(char*)&errorCode,(socklen_t*)&errorCodeLen);
	if(errorCode !=0)
	{
		close(transfer_sockfd);
		errormsg = "system error\r\n";
		return FALSE;
	}
	
	SmtpClient* pClientSmtp =  new SmtpClient(transfer_sockfd);
	if(!pClientSmtp)
		return FALSE;
	
	if(!pClientSmtp->Do_Ehlo_Command(NULL, errormsg))
	{
		delete pClientSmtp;
		close(transfer_sockfd);
		return FALSE;
	}
	if(pClientSmtp->StartTLS())
	{
		if(!pClientSmtp->Do_StartTLS_Command(errormsg))
		{
			delete pClientSmtp;
			close(transfer_sockfd);
			return FALSE;

		}
	}	
	if(!pClientSmtp->Do_Mail_Command(fromaddr, errormsg))
	{
		delete pClientSmtp;
		close(transfer_sockfd);
		return FALSE;
	}
	
	if(!pClientSmtp->Do_Rcpt_Command(toaddr, errormsg))
	{
		delete pClientSmtp;
		close(transfer_sockfd);
		return FALSE;
	}
	
	if(!pClientSmtp->Do_Data_Command(errormsg))
	{
		delete pClientSmtp;
		close(transfer_sockfd);
		return FALSE;
	}
	
	string emlfile;
	mailStg->GetMailIndex(mid, emlfile);
	
	MailLetter* Letter = new MailLetter(mailStg, CMailBase::m_private_path.c_str(), CMailBase::m_encoding.c_str(), memcached, emlfile.c_str());
	if(Letter)
    {
        if(Letter->GetSize() > 0)
        {
            int llen;
            char* lbuf = Letter->Body(llen);
            int wlen = 0;
            while(1)
            {
                if(!pClientSmtp->Do_Dataing_Command(lbuf + wlen, (llen - wlen) > 1448 ? 1448 : (llen - wlen), errormsg))
                    break;
                wlen += (llen - wlen) > 1448 ? 1448 : (llen - wlen);
                if(wlen >= llen)
                    break;
            }
        }
        Letter->Close();
        delete Letter;
    }
	if(!pClientSmtp->Do_Finish_Data_Command(errormsg))
	{
		delete pClientSmtp;
		close(transfer_sockfd);
		return FALSE;
	}
	pClientSmtp->Do_Quit_Command(errormsg);
	delete pClientSmtp;
	close(transfer_sockfd);
	return TRUE;
}


static BOOL RelayMail(MailStorage* mailStg, memcached_st * memcached, int mid, const char* uniqid, const char *mail_from, const char *rcpt_to, string& errormsg)
{
	string svraddr;
	strcut(rcpt_to, "@", NULL, svraddr);
	vector<Mx_List> list;
	udpdns dns(CMailBase::m_dns_server.c_str());

	int y;
	for(y = 0; y < 5; y ++)
	{
		list.clear();
		if(dns.mxquery(svraddr.c_str(), list) == 0)
			break;
	}
	
	if(y >= 5)
	{
		Mx_List ml;
		strcpy(ml.address, svraddr.c_str());
		ml.level = 0;
		list.push_back(ml);
	}
	errormsg = "";
	int mxcount = list.size();

	for(int x = 0; x < mxcount; x++)
	{
		if(SendMail(mailStg, memcached, list[x].address, "", rcpt_to, mid, errormsg) == TRUE)
		{
			return TRUE;
		}
	}
	if(mxcount == 0)
	{
		errormsg = "Can not get any MX record.";
	}
	return FALSE;
}


static void init_relay_handler()
{
	s_relay_stop = FALSE;
	s_relay_count = 0;
	
	pthread_mutex_init(&gs_thread_pool_mutex, NULL);
	sem_init(&gs_thread_pool_sem, 0, 0);
}

static void* begin_relay_handler(void* arg)
{	
	struct timespec ts;
	MailStorage* mailStg;
	ReplyInfo* reply_info;
	
	s_relay_count++;
	
	while(!s_relay_stop || !gs_thread_pool_arg_queue.empty())
	{
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 1;
		
		if(sem_timedwait(&gs_thread_pool_sem, &ts) == 0)
		{
			pthread_mutex_lock(&gs_thread_pool_mutex);
			if(!gs_thread_pool_arg_queue.empty())
			{
				reply_info = gs_thread_pool_arg_queue.front();
				gs_thread_pool_arg_queue.pop();
			}
			pthread_mutex_unlock(&gs_thread_pool_mutex);
			if(reply_info)
			{
				StorageEngineInstance stgengine_instance(reply_info->_storageEngine, &mailStg);			
				if(!mailStg)
				{
					continue;
				}
				
				mailStg->EntryThread();
				string errormsg;
				
				if(RelayMail(mailStg, reply_info->_memcached, reply_info->mail_info.mid, reply_info->mail_info.uniqid, reply_info->mail_info.mailfrom.c_str(), 
					reply_info->mail_info.rcptto.c_str(), errormsg) == FALSE)
				{
					fprintf(stderr, "%s", errormsg.c_str());
					
					mailStg->KeepLive();
					ReturnMail(mailStg, reply_info->_memcached, reply_info->mail_info.mid, reply_info->mail_info.uniqid, reply_info->mail_info.mailfrom.c_str(), 
						reply_info->mail_info.rcptto.c_str(), errormsg.c_str());
				}

				mailStg->ShitDelMail(reply_info->mail_info.mid);
				mailStg->LeaveThread();
				stgengine_instance.Release();		
				delete reply_info;
			}
		}
	}
	
	s_relay_count--;
	
	pthread_exit(0);
}

static void exit_relay_handler()
{
	unsigned long timeout = 20000;
	
	while(s_relay_count > 0 && timeout > 0)
	{
		usleep(1000);
		timeout--;
	}

	pthread_mutex_destroy(&gs_thread_pool_mutex);
	sem_close(&gs_thread_pool_sem);
}

static void clear_posix_queue(mqd_t qid)
{
	mq_attr attr;
	struct timespec ts;
	mq_getattr(qid, &attr);
	char* buf = (char*)malloc(attr.mq_msgsize);
	while(1)
	{
		clock_gettime(CLOCK_REALTIME, &ts);
		if(mq_timedreceive(qid, (char*)buf, attr.mq_msgsize, NULL, &ts) == -1)
		{
			break;
		}
	}
	free(buf);
}

/////////////////////////////////////////////////////////////////////////////////////////
MTA::MTA()
{
	char sztmp[128];
	sprintf(sztmp, "erisemail_%s", MTA_SERVICE_NAME);
	m_mta_name = sztmp;
	m_memcached = NULL;
}

MTA::~MTA()
{	
	
}

void MTA::ReloadConfig()
{
	string strqueue = "/.";
	strqueue += m_mta_name;
	strqueue += "_queue";

	string strsem = "/.";
	strsem += m_mta_name;
	strsem += "_lock";
	
	m_mta_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_mta_sid = sem_open(strsem.c_str(), O_RDWR);

	if(m_mta_qid == (mqd_t)-1 || m_mta_sid == SEM_FAILED)
		return;

	stQueueMsg qMsg;
	qMsg.cmd = MSG_GLOBAL_RELOAD;
	sem_wait(m_mta_sid);
	mq_send(m_mta_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_mta_sid);
	
	if(m_mta_qid != (mqd_t)-1)
		mq_close(m_mta_qid);
	if(m_mta_sid != SEM_FAILED)
		sem_close(m_mta_sid);

	printf("Reload %s Service OK\n", MTA_SERVICE_NAME);
}

void MTA::Stop()
{
	string strqueue = "/.";
	strqueue += m_mta_name;
	strqueue += "_queue";

	string strsem = "/.";
	strsem += m_mta_name;
	strsem += "_lock";

	m_mta_qid = mq_open(strqueue.c_str(), O_RDWR);
	m_mta_sid = sem_open(strsem.c_str(), O_RDWR);

	if(m_mta_qid == (mqd_t)-1 || m_mta_sid == SEM_FAILED)
		return;
	stQueueMsg qMsg;
	qMsg.cmd = MSG_EXIT;
	sem_wait(m_mta_sid);
	mq_send(m_mta_qid, (const char*)&qMsg, sizeof(stQueueMsg), 0);
	sem_post(m_mta_sid);
	if(m_mta_qid != (mqd_t)-1)
		mq_close(m_mta_qid);
	if(m_mta_sid != SEM_FAILED)
		sem_close(m_mta_sid);
	if(m_memcached)
	  memcached_free(m_memcached);
	m_memcached = NULL;
    
    //wake it up when block on POST_NOTIFY
    sem_t* post_sid = sem_open(".ERISEMAIL_POST_NOTIFY", O_RDWR);
    if(post_sid != SEM_FAILED)
    { 
        sem_post(post_sid);
        sem_close(post_sid);
    }
    
	printf("Stop %s Service OK\n", MTA_SERVICE_NAME);
}

#define MTA_QUERY_MAX_INTERVAL  5

int MTA::Run(int fd)
{		
	ThreadPool* thd_pool;
	int retVal = 0;
	do
	{
		memcached_server_st * memcached_servers = NULL;
		memcached_return memc_rc;
		m_memcached = NULL;
		for (map<string, int>::iterator iter = CMailBase::m_memcached_list.begin( ); iter != CMailBase::m_memcached_list.end( ); ++iter)
		{
            if(!m_memcached)
                m_memcached = memcached_create(NULL);
			memcached_servers = memcached_server_list_append(memcached_servers, (*iter).first.c_str(), (*iter).second, &memc_rc);
			memc_rc = memcached_server_push(m_memcached, memcached_servers);
		}
	
		m_storageEngine = new StorageEngine(CMailBase::m_db_host.c_str(), CMailBase::m_db_username.c_str(), CMailBase::m_db_password.c_str(), 
			CMailBase::m_db_name.c_str(), CMailBase::m_db_max_conn, CMailBase::m_db_port, CMailBase::m_db_sock_file.c_str(), CMailBase::m_private_path.c_str(), CMailBase::m_encoding.c_str(), m_memcached);
		if(!m_storageEngine)
		{	
			retVal = -1;
			break;
		}
        
        StorageEngineInstance* stgengine_instance = NULL;
        MailStorage* mailStg = NULL;
        
        stgengine_instance = new StorageEngineInstance(m_storageEngine, &mailStg);
        if(!stgengine_instance || !mailStg)
        {
            if(stgengine_instance)
                delete stgengine_instance;
            retVal = -1;
            break;
        }
        
        mailStg->InsertMTA(CMailBase::m_localhostname.c_str());

        stgengine_instance->Release();
        delete stgengine_instance;
        
		unsigned int result  = 0;

		string strqueue = "/.";
		strqueue += m_mta_name;
		strqueue += "_queue";

		string strsem = "/.";
		strsem += m_mta_name;
		strsem += "_lock";

		mq_attr attr;
		attr.mq_maxmsg = 8;
		attr.mq_msgsize = 1448; 
		attr.mq_flags = 0;
		
		m_mta_qid = mq_open(strqueue.c_str(), O_CREAT|O_RDWR, 0644, &attr);
		m_mta_sid = sem_open(strsem.c_str(), O_CREAT|O_RDWR, 0644, 1);
		
		sem_t * postmail_sid = sem_open(".ERISEMAIL_POST_NOTIFY", O_CREAT|O_RDWR, 0666, 0);
		if((m_mta_qid == (mqd_t)-1) || (m_mta_sid ==  SEM_FAILED) || postmail_sid == SEM_FAILED)
		{
			if(m_mta_sid != SEM_FAILED)
				sem_close(m_mta_sid);
		
			if(m_mta_qid != (mqd_t)-1)
				mq_close(m_mta_qid);
			
			sem_unlink(strsem.c_str());
			mq_unlink(strqueue.c_str());
			
			result = 1;
			write(fd, &result, sizeof(unsigned int));
			close(fd);
			retVal = -1;
			break;
			
		}
		clear_posix_queue(m_mta_qid);

		result = 0;
		write(fd, &result, sizeof(unsigned int));
		close(fd);
		
		int q_buf_len = attr.mq_msgsize;
		char* q_buf_ptr = (char*)malloc(q_buf_len);
		struct timespec ts1, ts2;
		stQueueMsg* pQMsg;
		int rc;

		thd_pool = new ThreadPool(CMailBase::m_mta_relaytasknum, init_relay_handler, begin_relay_handler, NULL, exit_relay_handler);

		while(1)
		{
			clock_gettime(CLOCK_REALTIME, &ts1);
			ts1.tv_sec += 1;
			rc = mq_timedreceive(m_mta_qid, q_buf_ptr, q_buf_len, 0, &ts1);
			if( rc != -1)
			{
				pQMsg = (stQueueMsg*)q_buf_ptr;
				if(pQMsg->cmd == MSG_EXIT)
				{
					s_relay_stop = TRUE;
					break;
				}
				else if(pQMsg->cmd == MSG_GLOBAL_RELOAD)
				{
					CMailBase::UnLoadConfig();
					CMailBase::LoadConfig();
				}
			}
			else
			{
				if((errno != ETIMEDOUT)&&(errno != EINTR)&&(errno != EMSGSIZE))
				{
					perror("");
					break;
				}
			}
            clock_gettime(CLOCK_REALTIME, &ts2);
            ts2.tv_sec += MTA_QUERY_MAX_INTERVAL;
            int sr = sem_timedwait(postmail_sid, &ts2);
			if( sr == 0 || (sr == -1 && errno == ETIMEDOUT))
			{
                stgengine_instance = NULL;
                mailStg = NULL;
        
				stgengine_instance = new StorageEngineInstance(m_storageEngine, &mailStg);
				if(!stgengine_instance || !mailStg)
				{
                    if(stgengine_instance)
                        delete stgengine_instance;
					continue;
				}		
				vector<Mail_Info> mitbl;
                mailStg->UpdateMTA(CMailBase::m_localhostname.c_str());
                
                unsigned int mta_index, mta_count;
                
                mailStg->GetMTAIndex(CMailBase::m_localhostname.c_str(), 4*MTA_QUERY_MAX_INTERVAL, mta_index, mta_count);
                
				if(mta_count > 0 && mailStg->ListExternMail(mitbl, mta_index, mta_count, CMailBase::m_mta_relaytasknum) == 0)
				{
                    for(int x = 0; x < mitbl.size(); x++)
                    {
                        ReplyInfo* reply_info = new ReplyInfo;
                        
                        reply_info->mail_info.mid = mitbl[x].mid;
                        memcpy(reply_info->mail_info.uniqid, mitbl[x].uniqid, 256);
                        reply_info->mail_info.mailfrom = mitbl[x].mailfrom;
                        reply_info->mail_info.rcptto = mitbl[x].rcptto;
                        reply_info->mail_info.mtime = mitbl[x].mtime;
                        reply_info->mail_info.mstatus = mitbl[x].mstatus;
                        reply_info->mail_info.mtype = mitbl[x].mtype;
                        reply_info->mail_info.mdid = mitbl[x].mdid;
                        reply_info->mail_info.length = mitbl[x].length;
                        reply_info->mail_info.reserve = mitbl[x].reserve;
                        reply_info->_storageEngine = m_storageEngine;
                        reply_info->_memcached = m_memcached;
                        pthread_mutex_lock(&gs_thread_pool_mutex);
                        gs_thread_pool_arg_queue.push(reply_info);
                        pthread_mutex_unlock(&gs_thread_pool_mutex);
                    
                        sem_post(&gs_thread_pool_sem);
                        
                        mailStg->Prefoward(mitbl[x].mid);
                    }
				}
				stgengine_instance->Release();
                delete stgengine_instance;
        
			}
		}
        
        free(q_buf_ptr);
        
		if(postmail_sid != SEM_FAILED)
  		{
            sem_close(postmail_sid);
		}
   
        stgengine_instance = NULL;
        mailStg = NULL;
        stgengine_instance = new StorageEngineInstance(m_storageEngine, &mailStg);
        if(stgengine_instance && mailStg)
        {
            mailStg->DeleteMTA(CMailBase::m_localhostname.c_str());
        }
        
        if(stgengine_instance)
            delete stgengine_instance;
        
		if(thd_pool)
			delete thd_pool;
		
		if(m_storageEngine)
			delete m_storageEngine;
		
        if(m_memcached)
            memcached_free(m_memcached);
    
        if(memcached_servers)
            memcached_server_list_free(memcached_servers);
    
		if(m_mta_sid != SEM_FAILED)
			sem_close(m_mta_sid);
		
		if(m_mta_qid != (mqd_t) -1)
			mq_close(m_mta_qid);

		sem_unlink(strsem.c_str());
		mq_unlink(strqueue.c_str());
	}while(0);
        
	return retVal;
}
