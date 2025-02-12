#include "http.h"

#include <base/math.h>
#include <base/system.h>
#include <engine/engine.h>
#include <engine/external/json-parser/json.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <game/version.h>

#if !defined(CONF_FAMILY_WINDOWS)
#include <csignal>
#endif

#define WIN32_LEAN_AND_MEAN
#include <curl/curl.h>
#include <curl/easy.h>

// TODO: Non-global pls?
static CURLSH *gs_Share;
static LOCK gs_aLocks[CURL_LOCK_DATA_LAST + 1];

static int GetLockIndex(int Data)
{
	if(!(0 <= Data && Data < CURL_LOCK_DATA_LAST))
	{
		Data = CURL_LOCK_DATA_LAST;
	}
	return Data;
}

static void CurlLock(CURL *pHandle, curl_lock_data Data, curl_lock_access Access, void *pUser) ACQUIRE(gs_aLocks[GetLockIndex(Data)])
{
	(void)pHandle;
	(void)Access;
	(void)pUser;
	lock_wait(gs_aLocks[GetLockIndex(Data)]);
}

static void CurlUnlock(CURL *pHandle, curl_lock_data Data, void *pUser) RELEASE(gs_aLocks[GetLockIndex(Data)])
{
	(void)pHandle;
	(void)pUser;
	lock_unlock(gs_aLocks[GetLockIndex(Data)]);
}

bool HttpInit(IStorage *pStorage)
{
	if(curl_global_init(CURL_GLOBAL_DEFAULT))
	{
		return true;
	}
	gs_Share = curl_share_init();
	if(!gs_Share)
	{
		return true;
	}
	// print curl version
	{
		curl_version_info_data *pVersion = curl_version_info(CURLVERSION_NOW);
		dbg_msg("http", "libcurl version %s (compiled = " LIBCURL_VERSION ")", pVersion->version);
	}

	for(auto &Lock : gs_aLocks)
	{
		Lock = lock_create();
	}
	curl_share_setopt(gs_Share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	curl_share_setopt(gs_Share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
	curl_share_setopt(gs_Share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
	curl_share_setopt(gs_Share, CURLSHOPT_LOCKFUNC, CurlLock);
	curl_share_setopt(gs_Share, CURLSHOPT_UNLOCKFUNC, CurlUnlock);

#if !defined(CONF_FAMILY_WINDOWS)
	// As a multithreaded application we have to tell curl to not install signal
	// handlers and instead ignore SIGPIPE from OpenSSL ourselves.
	signal(SIGPIPE, SIG_IGN);
#endif

	return false;
}

void EscapeUrl(char *pBuf, int Size, const char *pStr)
{
	char *pEsc = curl_easy_escape(0, pStr, 0);
	str_copy(pBuf, pEsc, Size);
	curl_free(pEsc);
}

CHttpRequest::CHttpRequest(const char *pUrl)
{
	str_copy(m_aUrl, pUrl, sizeof(m_aUrl));
}

CHttpRequest::~CHttpRequest()
{
	if(!m_WriteToFile)
	{
		m_BufferSize = 0;
		m_BufferLength = 0;
		free(m_pBuffer);
		m_pBuffer = nullptr;
	}
	curl_slist_free_all((curl_slist *)m_pHeaders);
	m_pHeaders = nullptr;
	if(m_pBody)
	{
		m_BodyLength = 0;
		free(m_pBody);
		m_pBody = nullptr;
	}
}

void CHttpRequest::Run()
{
	int FinalState;
	if(!BeforeInit())
	{
		FinalState = HTTP_ERROR;
	}
	else
	{
		CURL *pHandle = curl_easy_init();
		FinalState = RunImpl(pHandle);
		curl_easy_cleanup(pHandle);
	}

	m_State = OnCompletion(FinalState);
}

bool CHttpRequest::BeforeInit()
{
	if(m_WriteToFile)
	{
		if(fs_makedir_rec_for(m_aDestAbsolute) < 0)
		{
			dbg_msg("http", "i/o error, cannot create folder for: %s", m_aDest);
			return false;
		}

		m_File = io_open(m_aDestAbsolute, IOFLAG_WRITE);
		if(!m_File)
		{
			dbg_msg("http", "i/o error, cannot open file: %s", m_aDest);
			return false;
		}
	}
	return true;
}

int CHttpRequest::RunImpl(CURL *pUser)
{
	CURL *pHandle = (CURL *)pUser;
	if(!pHandle)
	{
		return HTTP_ERROR;
	}

	if(g_Config.m_DbgCurl)
	{
		curl_easy_setopt(pHandle, CURLOPT_VERBOSE, 1L);
	}
	char aErr[CURL_ERROR_SIZE];
	curl_easy_setopt(pHandle, CURLOPT_ERRORBUFFER, aErr);

	curl_easy_setopt(pHandle, CURLOPT_CONNECTTIMEOUT_MS, m_Timeout.ConnectTimeoutMs);
	curl_easy_setopt(pHandle, CURLOPT_LOW_SPEED_LIMIT, m_Timeout.LowSpeedLimit);
	curl_easy_setopt(pHandle, CURLOPT_LOW_SPEED_TIME, m_Timeout.LowSpeedTime);

	curl_easy_setopt(pHandle, CURLOPT_SHARE, gs_Share);
	curl_easy_setopt(pHandle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	curl_easy_setopt(pHandle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(pHandle, CURLOPT_MAXREDIRS, 4L);
	curl_easy_setopt(pHandle, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(pHandle, CURLOPT_URL, m_aUrl);
	curl_easy_setopt(pHandle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(pHandle, CURLOPT_USERAGENT, GAME_NAME " " GAME_RELEASE_VERSION " (" CONF_PLATFORM_STRING "; " CONF_ARCH_STRING ")");
	curl_easy_setopt(pHandle, CURLOPT_ACCEPT_ENCODING, ""); // Use any compression algorithm supported by libcurl.

	curl_easy_setopt(pHandle, CURLOPT_WRITEDATA, this);
	curl_easy_setopt(pHandle, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(pHandle, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(pHandle, CURLOPT_PROGRESSDATA, this);
	curl_easy_setopt(pHandle, CURLOPT_PROGRESSFUNCTION, ProgressCallback);
	curl_easy_setopt(pHandle, CURLOPT_IPRESOLVE, m_IpResolve == IPRESOLVE::V4 ? CURL_IPRESOLVE_V4 : m_IpResolve == IPRESOLVE::V6 ? CURL_IPRESOLVE_V6 : CURL_IPRESOLVE_WHATEVER);

	if(curl_version_info(CURLVERSION_NOW)->version_num < 0x074400)
	{
		// Causes crashes, see https://github.com/ddnet/ddnet/issues/4342.
		// No longer a problem in curl 7.68 and above, and 0x44 = 68.
		curl_easy_setopt(pHandle, CURLOPT_FORBID_REUSE, 1L);
	}

#ifdef CONF_PLATFORM_ANDROID
	curl_easy_setopt(pHandle, CURLOPT_CAINFO, "data/cacert.pem");
#endif

	switch(m_Type)
	{
	case REQUEST::GET:
		break;
	case REQUEST::HEAD:
		curl_easy_setopt(pHandle, CURLOPT_NOBODY, 1L);
		break;
	case REQUEST::POST:
	case REQUEST::POST_JSON:
		if(m_Type == REQUEST::POST_JSON)
		{
			Header("Content-Type: application/json");
		}
		curl_easy_setopt(pHandle, CURLOPT_POSTFIELDS, m_pBody);
		curl_easy_setopt(pHandle, CURLOPT_POSTFIELDSIZE, m_BodyLength);
		break;
	}

	curl_easy_setopt(pHandle, CURLOPT_HTTPHEADER, m_pHeaders);

	if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::ALL)
		dbg_msg("http", "fetching %s", m_aUrl);
	m_State = HTTP_RUNNING;
	int Ret = curl_easy_perform(pHandle);
	if(Ret != CURLE_OK)
	{
		if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::FAILURE)
			dbg_msg("http", "%s failed. libcurl error: %s", m_aUrl, aErr);
		return (Ret == CURLE_ABORTED_BY_CALLBACK) ? HTTP_ABORTED : HTTP_ERROR;
	}
	else
	{
		if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::ALL)
			dbg_msg("http", "task done %s", m_aUrl);
		return HTTP_DONE;
	}
}

size_t CHttpRequest::OnData(char *pData, size_t DataSize)
{
	if(!m_WriteToFile)
	{
		if(DataSize == 0)
		{
			return DataSize;
		}
		size_t NewBufferSize = maximum((size_t)1024, m_BufferSize);
		while(m_BufferLength + DataSize > NewBufferSize)
		{
			NewBufferSize *= 2;
		}
		if(NewBufferSize != m_BufferSize)
		{
			m_pBuffer = (unsigned char *)realloc(m_pBuffer, NewBufferSize);
			m_BufferSize = NewBufferSize;
		}
		mem_copy(m_pBuffer + m_BufferLength, pData, DataSize);
		m_BufferLength += DataSize;
		return DataSize;
	}
	else
	{
		return io_write(m_File, pData, DataSize);
	}
}

size_t CHttpRequest::WriteCallback(char *pData, size_t Size, size_t Number, void *pUser)
{
	return ((CHttpRequest *)pUser)->OnData(pData, Size * Number);
}

int CHttpRequest::ProgressCallback(void *pUser, double DlTotal, double DlCurr, double UlTotal, double UlCurr)
{
	CHttpRequest *pTask = (CHttpRequest *)pUser;
	pTask->m_Current.store(DlCurr, std::memory_order_relaxed);
	pTask->m_Size.store(DlTotal, std::memory_order_relaxed);
	pTask->m_Progress.store((100 * DlCurr) / (DlTotal ? DlTotal : 1), std::memory_order_relaxed);
	pTask->OnProgress();
	return pTask->m_Abort ? -1 : 0;
}

int CHttpRequest::OnCompletion(int State)
{
	if(m_WriteToFile)
	{
		if(m_File && io_close(m_File) != 0)
		{
			dbg_msg("http", "i/o error, cannot close file: %s", m_aDest);
			State = HTTP_ERROR;
		}

		if(State == HTTP_ERROR || State == HTTP_ABORTED)
		{
			fs_remove(m_aDestAbsolute);
		}
	}
	return State;
}

void CHttpRequest::WriteToFile(IStorage *pStorage, const char *pDest, int StorageType)
{
	m_WriteToFile = true;
	str_copy(m_aDest, pDest, sizeof(m_aDest));
	if(StorageType == -2)
	{
		pStorage->GetBinaryPath(m_aDest, m_aDestAbsolute, sizeof(m_aDestAbsolute));
	}
	else
	{
		pStorage->GetCompletePath(StorageType, m_aDest, m_aDestAbsolute, sizeof(m_aDestAbsolute));
	}
}

void CHttpRequest::Header(const char *pNameColonValue)
{
	m_pHeaders = curl_slist_append((curl_slist *)m_pHeaders, pNameColonValue);
}

void CHttpRequest::Result(unsigned char **ppResult, size_t *pResultLength) const
{
	if(m_WriteToFile || State() != HTTP_DONE)
	{
		*ppResult = nullptr;
		*pResultLength = 0;
		return;
	}
	*ppResult = m_pBuffer;
	*pResultLength = m_BufferLength;
}

json_value *CHttpRequest::ResultJson() const
{
	unsigned char *pResult;
	size_t ResultLength;
	Result(&pResult, &ResultLength);
	if(!pResult)
	{
		return nullptr;
	}
	return json_parse((char *)pResult, ResultLength);
}
