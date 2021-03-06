#include "mapinfo.h"
#include "ShareMemory.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include "soapH.h"
#include "httppost.h"
#include "json.h"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h" 

using namespace rapidjson;

#define BACKLOG (100)
#define MAX_THR (10)
#define MAX_QUEUE (1000)

#define HTTP_PORT 10000

#define JSON_FINDPOS_TEXT "\"Msisdn\":\"%s\",\"Latitude\":\"%f\",\"Longitude\":\"%f\",\"CurrTime\":\"%ld\""

SOAP_SOCKET queue[MAX_QUEUE];

//全局函数
CMapInfo g_objMapInfo;

void Gdaemon()
{
	pid_t pid;

	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);
	signal(SIGTSTP,SIG_IGN);

	if(setpgrp() == -1)
	{	
		perror("setpgrp failure");
	}

	signal(SIGHUP,SIG_IGN);

	if((pid = fork()) < 0)
	{	
		perror("fork failure");
		exit(1);
	}
	else if(pid > 0)
	{
		exit(0);
	}

	setsid();
	umask(0);

	signal(SIGCLD,SIG_IGN);
	signal(SIGCHLD,SIG_IGN);
	signal(SIGPIPE,SIG_IGN);
}

//写独占文件锁
int AcquireWriteLock(int fd, int start, int len)
{
	struct flock arg;
	arg.l_type = F_WRLCK; // 加写锁
	arg.l_whence = SEEK_SET;
	arg.l_start = start;
	arg.l_len = len;
	arg.l_pid = getpid();

	return fcntl(fd, F_SETLKW, &arg);
}

//释放独占文件锁
int ReleaseLock(int fd, int start, int len)
{
	struct flock arg;
	arg.l_type = F_UNLCK; //  解锁
	arg.l_whence = SEEK_SET;
	arg.l_start = start;
	arg.l_len = len;
	arg.l_pid = getpid();

	return fcntl(fd, F_SETLKW, &arg);
}

//查看写锁
int SeeLock(int fd, int start, int len)
{
	struct flock arg;
	arg.l_type = F_WRLCK;
	arg.l_whence = SEEK_SET;
	arg.l_start = start;
	arg.l_len = len;
	arg.l_pid = getpid();

	if (fcntl(fd, F_GETLK, &arg) != 0) // 获取锁
	{
		return -1; // 测试失败
	}

	if (arg.l_type == F_UNLCK)
	{
		return 0; // 无锁
	}
	else if (arg.l_type == F_RDLCK)
	{
		return 1; // 读锁
	}
	else if (arg.l_type == F_WRLCK)
	{
		return 2; // 写所
	}

	return 0;
}

int head = 0;
int tail = 0;
pthread_mutex_t queue_cs;
pthread_cond_t queue_cv;

static SOAP_SOCKET dequeue();
static int enqueue(SOAP_SOCKET sock);
static void *process_queue(void *soap);
int text_post_handler(struct soap *soap);
int sendPost(char *url,char *cont);

SOAP_NMAC struct Namespace namespaces[] =
{
	{"SOAP-ENV", "http://schemas.xmlsoap.org/soap/envelope/", "http://www.w3.org/*/soap-envelope", NULL},
	{"SOAP-ENC", "http://schemas.xmlsoap.org/soap/encoding/", "http://www.w3.org/*/soap-encoding", NULL},
	{"xsi", "http://www.w3.org/2001/XMLSchema-instance", "http://www.w3.org/*/XMLSchema-instance", NULL},
	{"xsd", "http://www.w3.org/2001/XMLSchema", "http://www.w3.org/*/XMLSchema", NULL},
	{NULL, NULL, NULL, NULL}
};

int __ns1__add(struct soap *soap, struct ns2__pair *in, double *out)
{ *out = in->a + in->b;
  return SOAP_OK;
}

int __ns1__sub(struct soap *soap, struct ns2__pair *in, double *out)
{ *out = in->a - in->b;
  return SOAP_OK;
}

int __ns1__mul(struct soap *soap, struct ns2__pair *in, double *out)
{ *out = in->a * in->b;
  return SOAP_OK;
}

int __ns1__div(struct soap *soap, struct ns2__pair *in, double *out)
{ *out = in->a / in->b;
  return SOAP_OK;
}

int __ns1__pow(struct soap *soap, struct ns2__pair *in, double *out)
{ *out = pow(in->a, in->b);
  return SOAP_OK;
}

static int enqueue(SOAP_SOCKET sock)
{
    int status = SOAP_OK;
    int next;
    pthread_mutex_lock(&queue_cs);
    next = tail + 1;
    if (next >= MAX_QUEUE)
        next = 0;
    if (next == head)
        status = SOAP_EOM;
    else
    {
        queue[tail] = sock;
        tail = next;
    }
    pthread_cond_signal(&queue_cv);
    pthread_mutex_unlock(&queue_cs);
    return status;
}

static SOAP_SOCKET dequeue()
{
    SOAP_SOCKET sock;
    pthread_mutex_lock(&queue_cs);
    while (head == tail)
        pthread_cond_wait(&queue_cv, &queue_cs);
    sock = queue[head++];
    if (head >= MAX_QUEUE)
        head = 0;
    pthread_mutex_unlock(&queue_cs);
    return sock;
}

static void *process_queue(void *soap)
{
    struct soap *tsoap = (struct soap*) soap;
    for (;;)
    {
        tsoap->socket = dequeue();
        if (!soap_valid_socket(tsoap->socket))
            break;
        soap_serve(tsoap);
        soap_destroy(tsoap);
        soap_end(tsoap);
    }
    return NULL;
}

int text_post_handler(struct soap *soap)
{ 
	//处理Json字符串
	Document document;	
	
	char *buf;
	char retBuf[2048] = {'\0'};
	memset(retBuf,0,sizeof(retBuf));
  size_t len;
  soap_http_body(soap, &buf, &len);
  printf("[text_post_handler]buf=%s.\n", buf);
  printf("[text_post_handler]path=%s.\n", soap->path);
	
	//解析Json, 获得查询相关参数
	if(false == document.Parse(buf).HasParseError())
	{
		double dLatitude  = 0.0f;
		double dLongitude = 0.0f;
		double dRadius    = 0.0f;		
		if(strcmp(soap->path, "/GeoHash/Search/") == 0)
		{
			//解析GeoHash查询功能参数
			if(document.HasMember("Latitude") ==  true)
			{
				dLatitude = atof(document["Latitude"].GetString());
			}
			if(document.HasMember("Longitude") ==  true)
			{
				dLongitude = atof(document["Longitude"].GetString());
			}
			if(document.HasMember("Radius") ==  true)
			{
				dRadius = atof(document["Radius"].GetString());
			}
			
			if(dLatitude != 0.0f && dLongitude != 0.0f && dRadius != 0.0f)
			{
				//这里添加geo调用算法
				vector<_Pos_Info*> vecPosList;
			 	bool blState = g_objMapInfo.FindPos(dLatitude, dLongitude, dRadius, vecPosList);
			 	
			 	if(true == blState)
			 	{
			 		//将结果拼装成Json
			 		char szTemp[500] = {'\0'};
			 		retBuf[0] = '{';
			 		int nPos  = 1;
			 		for(int i = 0; i < (int)vecPosList.size(); i++)
			 		{
			 			if(i != (int)vecPosList.size() - 1)
			 			{
			 				sprintf(szTemp, JSON_FINDPOS_TEXT, vecPosList[i]->m_szMsisdn,
 																								 vecPosList[i]->m_dPosLatitude,
 																								 vecPosList[i]->m_dPosLongitude,
 																								 vecPosList[i]->m_ttCurrTime);
 							int nLen = (int)strlen(szTemp);
 						  memcpy(&retBuf[nPos], szTemp, nLen);
 						  nPos += nLen;
 						  retBuf[nPos] = ',';
 						  nPos += 1;
 						}
 						else
 						{
			 				sprintf(szTemp, JSON_FINDPOS_TEXT, vecPosList[i]->m_szMsisdn,
 																								 vecPosList[i]->m_dPosLatitude,
 																								 vecPosList[i]->m_dPosLongitude,
 																								 vecPosList[i]->m_ttCurrTime);
 							int nLen = (int)strlen(szTemp);
 						  memcpy(&retBuf[nPos], szTemp, nLen);
 						  nPos += nLen;
 						  retBuf[nPos] = '}';
 						  nPos += 1; 							
 						}
			 		}
				}
				else
				{
					sprintf(retBuf, "{\"error\":\"2\"}");
				}
			}
			else
			{
				sprintf(retBuf, "{\"error\":\"1\"}");
			}				
		}
		else if(strcmp(soap->path, "/GeoHash/Add/") == 0)
		{
			char   szMsisdn[15]   = {'\0'};
			double dLatitude      = 0.0f;
			double dLongitude     = 0.0f;
			time_t ttNow          = 0;
						
			//解析添加当前点数据参数
			if(document.HasMember("Msisdn") ==  true)
			{
				sprintf(szMsisdn, "%s", document["Msisdn"].GetString());
			}			
			if(document.HasMember("Latitude") ==  true)
			{
				dLatitude = atof(document["Latitude"].GetString());
			}
			if(document.HasMember("Longitude") ==  true)
			{
				dLongitude = atof(document["Longitude"].GetString());
			}
			if(document.HasMember("Time") ==  true)
			{
				ttNow = (time_t)atol(document["Time"].GetString());
			}
			
			if(strlen(szMsisdn) > 0 && dLatitude != 0.0f && dLongitude != 0.0f && ttNow != 0)
			{
				bool blState = g_objMapInfo.AddPos(szMsisdn, dLatitude, dLongitude, ttNow);
				if(true == blState)
				{
					sprintf(retBuf, "{\"success\":\"0\"}");
				}
				else
				{
					sprintf(retBuf, "{\"error\":\"2\"}");
				}
			}
			else
			{
				sprintf(retBuf, "{\"error\":\"1\"}");
			}										
		}					
	}	
	
	printf("[text_post_handler]retBuf=%s.\n", retBuf);
  len = strlen(retBuf);
  soap_response(soap, SOAP_HTML);
  soap_send_raw(soap, retBuf, len);
  soap_end_send(soap);
  return SOAP_OK;
}

int Chlid_Run()
{
	//Http子进程
	size_t stShareSize = g_objMapInfo.GetSize(1000000);
	printf("[main]All Size=%d.\n", stShareSize); 
	
	int nPoolSize = 600000;
	shm_key obj_key = 30010;
	shm_id obj_shm_id;
	bool blCreate = true;		
	char* pData = Open_Share_Memory_API(obj_key, stShareSize, obj_shm_id, blCreate);
	//char* pData = new char[stShareSize];
	
	if(NULL != pData)
	{
		if(blCreate == true)
		{
			memset(pData, 0, stShareSize);
			g_objMapInfo.Init(pData);
		}
		else
		{
			g_objMapInfo.Load(pData);
		}
	}
	else
	{
		printf("[main]Create share memory is fail.\n");
		return 0;
	}
 	
	int ret = 0;
	char *buf;
  size_t len;
  struct soap soap;

  soap_init(&soap);
  soap_set_omode(&soap, SOAP_C_UTFSTRING);

  struct http_post_handlers handlers[] =
  { 
    { "text/*",    text_post_handler },
    { "text/*;*",  text_post_handler },
    { "POST",      text_post_handler },
    { NULL }
  };
  soap_register_plugin_arg(&soap, http_post, handlers); 
  struct soap *soap_thr[MAX_THR];
  pthread_t tid[MAX_THR];
  SOAP_SOCKET m, s;
  int i;

  m = soap_bind(&soap, NULL, HTTP_PORT, BACKLOG);
  if (!soap_valid_socket(m))
  {
      printf("soap_valid_socket\n");
 			return 1;
  }
 
  pthread_mutex_init(&queue_cs, NULL);
  pthread_cond_init(&queue_cv, NULL);
  for (i = 0; i < MAX_THR; i++)
  {
      soap_thr[i] = soap_copy(&soap);
      soap_set_mode(soap_thr[i], SOAP_C_UTFSTRING);
      pthread_create(&tid[i], NULL, (void*(*)(void*)) process_queue,(void*) soap_thr[i]);
  }
  
  for (;;)
  {
      s = soap_accept(&soap);
      if (!soap_valid_socket(s))
      {
          if (soap.errnum)
          {
              soap_print_fault(&soap, stderr);
              continue; 
          }
          else
          {  
              break;
          }
      }
      while (enqueue(s) == SOAP_EOM)
      {
          sleep(1);
      }
  }
 
  for (i = 0; i < MAX_THR; i++)
  {
      while (enqueue(SOAP_INVALID_SOCKET) == SOAP_EOM)
      {
          sleep(1);
      }
  }
 
  for (i = 0; i < MAX_THR; i++)
  {
      pthread_join(tid[i], NULL);
      soap_done(soap_thr[i]);
      free(soap_thr[i]);
  }
  
  pthread_mutex_destroy(&queue_cs);
  pthread_cond_destroy(&queue_cv);
 
  soap_done(&soap); 	
	
	//delete[] pData;
	return 0;
}

int main()
{
	//当前监控子线程个数
	int nNumChlid = 1;
	
	//检测时间间隔参数
	struct timespec tsRqt;
	
	//文件锁
	int fd_lock = 0;
	
	int nRet = 0;

	//主进程检测时间间隔（设置每隔5秒一次）
	tsRqt.tv_sec  = 5;
	tsRqt.tv_nsec = 0;
	
	//获得当前路径
	char szWorkDir[255] = {0};
  if(!getcwd(szWorkDir, 260))  
  {  
		exit(1);
  }
  printf("[Main]szWorkDir=%s.\n", szWorkDir);
	
	// 打开（创建）锁文件
	char szFileName[200] = {'\0'};
	memset(szFileName, 0, sizeof(flock));
	sprintf(szFileName, "%s/testwatch.lk", szWorkDir);
	fd_lock = open(szFileName, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
	if (fd_lock < 0)
	{
		printf("open the flock and exit, errno = %d.", errno);
		exit(1);
	}
	
	//查看当前文件锁是否已锁
	nRet = SeeLock(fd_lock, 0, sizeof(int));
	if (nRet == -1 || nRet == 2) 
	{
		printf("file is already exist!");
		exit(1);
	}
	
	//如果文件锁没锁，则锁住当前文件锁
	if (AcquireWriteLock(fd_lock, 0, sizeof(int)) != 0)
	{
		printf("lock the file failure and exit, idx = 0!.");
		exit(1);
	}
	
	//写入子进程锁信息
	lseek(fd_lock, 0, SEEK_SET);
	for (int nIndex = 0; nIndex <= nNumChlid; nIndex++)
	{
		write(fd_lock, &nIndex, sizeof(nIndex));
	}
	
	Gdaemon();
  
	while (1)
	{
		for (int nChlidIndex = 1; nChlidIndex <= nNumChlid; nChlidIndex++)
		{
			//测试每个子进程的锁是否还存在
			nRet = SeeLock(fd_lock, nChlidIndex * sizeof(int), sizeof(int));
			if (nRet == -1 || nRet == 2)
			{
				continue;
			}
			//如果文件锁没有被锁，则设置文件锁，并启动子进程
			int npid = fork();
			if (npid == 0)
			{
				//上文件锁
				if(AcquireWriteLock(fd_lock, nChlidIndex * sizeof(int), sizeof(int)) != 0)
				{
					printf("child %d AcquireWriteLock failure.\n", nChlidIndex);
					exit(1);
				}
				
				//启动子进程
				Chlid_Run();
				
				//子进程在执行完任务后必须退出循环和释放锁 
				ReleaseLock(fd_lock, nChlidIndex * sizeof(int), sizeof(int));	        
				}
			}
		
		printf("child count(%d) is ok.\n", nNumChlid);
		//检查间隔
		nanosleep(&tsRqt, NULL);
	}
	
	return 0;	
}
