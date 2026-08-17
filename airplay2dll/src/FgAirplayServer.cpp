#include "pch.h"
#include "FgAirplayServer.h"
#include "Airplay2Head.h"
#include <limits>
#include <thread>
#include <vector>
#include "CAutoLock.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

static BOOL GetPrimaryMacAddress(char strMac[6]);

FgAirplayServer::FgAirplayServer()
	: m_pCallback(NULL)
	, m_pDnsSd(NULL)
	, m_pAirplay(NULL)
	, m_pRaop(NULL)
	, m_fScaleRatio(1.0f)
{
	memset(&m_stAirplayCB, 0, sizeof(airplay_callbacks_t));
	memset(&m_stRaopCB, 0, sizeof(raop_callbacks_t));
	m_stAirplayCB.cls = this;
	m_stRaopCB.cls = this;

// 	m_stAirplayCB.audio_init = audio_init;
// 	m_stAirplayCB.audio_process = audio_process_ap;
// 	m_stAirplayCB.audio_flush = audio_flush;
// 	m_stAirplayCB.audio_destroy = audio_destroy;
	m_stAirplayCB.video_play = ap_video_play;
	m_stAirplayCB.video_get_play_info = ap_video_get_play_info;
	m_stAirplayCB.pin_request = pin_request;

	m_stRaopCB.connected = connected;
	m_stRaopCB.disconnected = disconnected;
	// m_stRaopCB.audio_init = audio_init;
	m_stRaopCB.audio_set_volume = audio_set_volume;
	m_stRaopCB.audio_set_metadata = audio_set_metadata;
	m_stRaopCB.audio_set_coverart = audio_set_coverart;
	m_stRaopCB.audio_process = audio_process;
	m_stRaopCB.audio_flush = audio_flush;
	// m_stRaopCB.audio_destroy = audio_destroy;
	m_stRaopCB.video_process = video_process;
	m_stRaopCB.pin_request = pin_request;

	m_mutexMap = CreateMutex(NULL, FALSE, NULL);
}

FgAirplayServer::~FgAirplayServer()
{
	m_pCallback = NULL;

	clearChannels();

	CloseHandle(m_mutexMap);
}

int FgAirplayServer::start(const char serverName[AIRPLAY_NAME_LEN], 
	unsigned int raopPort, unsigned int airplayPort,
	IAirServerCallback* callback, const char* password,
	unsigned int displayWidth, unsigned int displayHeight)
{
	m_pCallback = callback;
	const char* authPassword = (password != NULL && password[0] != '\0') ? password : NULL;

	unsigned short raop_port = raopPort;
	unsigned short airplay_port = airplayPort;
	char hwaddr[] = { 0x48, 0x5d, 0x60, 0x7c, 0xee, 0x22 };
	char* pemstr = NULL;

	int ret = 0;
	do {

		GetPrimaryMacAddress(hwaddr);

		m_pAirplay = airplay_init(10, &m_stAirplayCB, pemstr, &ret);
		if (m_pAirplay == NULL) {
			ret = -1;
			break;
		}
		ret = airplay_start(m_pAirplay, &airplay_port, hwaddr, sizeof(hwaddr), authPassword);
		if (ret < 0) {
			break;
		}
		airplay_set_log_level(m_pAirplay, RAOP_LOG_DEBUG);
		airplay_set_log_callback(m_pAirplay, &log_callback, this);

		m_pRaop = raop_init(10, &m_stRaopCB);
		if (m_pRaop == NULL) {
			ret = -1;
			break;
		}

		raop_set_log_level(m_pRaop, RAOP_LOG_DEBUG);
		raop_set_log_callback(m_pRaop, &log_callback, this);
		raop_set_password(m_pRaop, authPassword);
		raop_set_display_size(m_pRaop, displayWidth, displayHeight);
		ret = raop_start(m_pRaop, &raop_port);
		if (ret < 0) {
			break;
		}
		raop_set_port(m_pRaop, raop_port);

		m_pDnsSd = dnssd_init(&ret);
		if (m_pDnsSd == NULL) {
			ret = -1;
			break;
		}
		ret = dnssd_register_raop(m_pDnsSd, serverName, raop_port, hwaddr, sizeof(hwaddr),
			authPassword != NULL ? 1 : 0);
		if (ret < 0) {
			break;
		}
		ret = dnssd_register_airplay(m_pDnsSd, serverName, airplay_port, hwaddr, sizeof(hwaddr),
			authPassword != NULL ? 1 : 0);
		if (ret < 0) {
			break;
		}

		raop_log_info(m_pRaop, "Startup complete... Kill with Ctrl+C\n");
	} while (false);

	if (ret != 0) {
		stop();
	}

	return 0;
}

void FgAirplayServer::stop()
{
	// First, stop accepting new connections by unregistering DNS-SD
	if (m_pDnsSd) {
		dnssd_unregister_airplay(m_pDnsSd);
		dnssd_unregister_raop(m_pDnsSd);
		dnssd_destroy(m_pDnsSd);
		m_pDnsSd = NULL;
	}

	// Wait a bit to let any in-flight callbacks complete
	// This prevents crashes from callbacks accessing destroyed objects
	Sleep(100);

	// Now destroy the servers
	if (m_pRaop) {
		raop_destroy(m_pRaop);
// 		raop_set_log_callback(m_pRaop, &log_callback, NULL);
		m_pRaop = NULL;
	}

	if (m_pAirplay) {
		airplay_destroy(m_pAirplay);
// 		airplay_set_log_callback(m_pAirplay, &log_callback, NULL);
		m_pAirplay = NULL;
	}

	// Wait again to ensure all callbacks have finished
	Sleep(100);

	// Clear all channels
	clearChannels();
	
	// Finally, clear the callback pointer
	m_pCallback = NULL;
}

float FgAirplayServer::setScale(float fRatio)
{
	m_fScaleRatio = min(10, max(0.1, fRatio));

	FgAirplayChannelMap::iterator it;
	for (it = m_mapChannel.begin(); it != m_mapChannel.end(); ++it)
	{
		it->second->setScale(m_fScaleRatio);
	}
	return m_fScaleRatio;
}

void FgAirplayServer::clearChannels()
{
	CAutoLock oLock(m_mutexMap, "clearChannels");
	while (m_mapChannel.size() > 0)
	{
		FgAirplayChannelMap::iterator it = m_mapChannel.begin();
		it->second->release();
		m_mapChannel.erase(it);
	}
}

FgAirplayChannel* FgAirplayServer::getChannel(const char* remoteDeviceId)
{
	std::string deviceId(remoteDeviceId);
	FgAirplayChannel* pChannel = m_mapChannel[deviceId];
	if (NULL == pChannel)
	{
		pChannel = new FgAirplayChannel(m_pCallback);
		m_mapChannel[deviceId] = pChannel;
	}

	return pChannel;
}


void FgAirplayServer::connected(void* cls, const char* remoteName, const char* remoteDeviceId)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}
	CAutoLock oLock(pServer->m_mutexMap, "connected");
	pServer->getChannel(remoteDeviceId);

	if (pServer->m_pCallback != NULL)
	{
		pServer->m_pCallback->connected(remoteName, remoteDeviceId);
	}
}

void FgAirplayServer::disconnected(void* cls, const char* remoteName, const char* remoteDeviceId)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}
	
	// Safely call the callback
	if (pServer->m_pCallback != NULL)
	{
		pServer->m_pCallback->disconnected(remoteName, remoteDeviceId);
	}

	// Wait a bit to ensure any in-flight video/audio processing completes
	// This prevents accessing channels that are about to be deleted
	Sleep(50);

	// Now safely remove the channel
	FgAirplayChannel* pChannel = NULL;
	{
		CAutoLock oLock(pServer->m_mutexMap, "disconnected");
		std::string deviceId(remoteDeviceId);
		FgAirplayChannelMap::iterator it = pServer->m_mapChannel.find(deviceId);
		if (it != pServer->m_mapChannel.end()) {
			pChannel = it->second;
			pServer->m_mapChannel.erase(it);
		}
	}
	// Release outside the critical section to avoid holding lock too long
	if (pChannel) {
		pChannel->release();
	}
}

// void* FgAirplayServer::audio_init(void* opaque, int bits, int channels, int samplerate)
// {
// 	return nullptr;
// }

void FgAirplayServer::audio_set_volume(void* cls, void* session, float volume, const char* remoteName, const char* remoteDeviceId)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}

	if (pServer->m_pCallback != NULL)
	{
		// Forward volume to callback (volume is in dB: 0.0 = max, -144.0 = mute)
		pServer->m_pCallback->setVolume(volume, remoteName, remoteDeviceId);
	}
}

void FgAirplayServer::audio_set_metadata(void* cls, void* session, const void* buffer, int buflen, const char* remoteName, const char* remoteDeviceId)
{
}

void FgAirplayServer::audio_set_coverart(void* cls, void* session, const void* buffer, int buflen, const char* remoteName, const char* remoteDeviceId)
{
}

// void FgAirplayServer::audio_process_ap(void* cls, void* session, const void* buffer, int buflen)
// {
// }

void FgAirplayServer::audio_process(void* cls, pcm_data_struct* data, const char* remoteName, const char* remoteDeviceId)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}

	// Check if we're shutting down before processing audio
	if (!pServer->m_pRaop || !pServer->m_pAirplay || !pServer->m_pCallback)
	{
		return;
	}

	if (pServer->m_pCallback != NULL)
	{
		SFgAudioFrame* frame = new SFgAudioFrame();
		frame->bitsPerSample = data->bits_per_sample;
		frame->channels = data->channels;
		frame->pts = data->pts;
		frame->sampleRate = data->sample_rate;
		frame->dataLen = data->data_len;
		frame->data = new uint8_t[frame->dataLen];
		memcpy(frame->data, data->data, frame->dataLen);

		pServer->m_pCallback->outputAudio(frame, remoteName, remoteDeviceId);
		delete[] frame->data;
		delete frame;
	}
}

void FgAirplayServer::audio_flush(void* cls, void* session, const char* remoteName, const char* remoteDeviceId)
{
}

void FgAirplayServer::audio_destroy(void* cls, void* session, const char* remoteName, const char* remoteDeviceId)
{
}

void FgAirplayServer::video_process(void* cls, h264_decode_struct* h264data, const char* remoteName, const char* remoteDeviceId)
{

	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}
	
	// Check if we're shutting down
	if (!pServer->m_pRaop || !pServer->m_pAirplay)
	{
		return;
	}
	
	if (h264data->data_len <= 0)
	{
		return;
	}

	SFgH264Data* pData = new SFgH264Data();
	memset(pData, 0, sizeof(SFgH264Data));

	if (h264data->frame_type == 0)
	{
		pData->size = h264data->data_len;
		pData->data = new uint8_t[pData->size];
		pData->is_key = 1;
		memcpy(pData->data, h264data->data, h264data->data_len);
	}
	else if (h264data->frame_type == 1)
	{
		pData->size = h264data->data_len;
		pData->data = new uint8_t[pData->size];
		memcpy(pData->data, h264data->data, h264data->data_len);
	}

	FgAirplayChannel* pChannel = NULL;
	{
		CAutoLock oLock(pServer->m_mutexMap, "video_process");
		// Check again inside the lock in case we're shutting down
		if (!pServer->m_pCallback) {
			delete[] pData->data;
			delete pData;
			return;
		}
		pChannel = pServer->getChannel(remoteDeviceId);
		if (pChannel) {
			pChannel->addRef();
		}
	}
	if (pChannel)
	{
		pChannel->decodeH264Data(pData, remoteName, remoteDeviceId);
		pChannel->release();
	}
	delete[] pData->data;
	delete pData;
}

void FgAirplayServer::ap_video_play(void* cls, char* url, double volume, double start_pos)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}
	if (pServer->m_pCallback)
	{
		pServer->m_pCallback->videoPlay(url, volume, start_pos);
	}
}

void FgAirplayServer::ap_video_get_play_info(void* cls, double* duration, double* position, double* rate)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer)
	{
		return;
	}
	if (pServer->m_pCallback)
	{
		pServer->m_pCallback->videoGetPlayInfo(duration, position, rate);
	}
}

int FgAirplayServer::pin_request(void* cls, const char* remoteAddress, const char* pin)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (pServer == NULL || pServer->m_pCallback == NULL) {
		return 0;
	}
	return pServer->m_pCallback->requestPinApproval(remoteAddress, pin) ? 1 : 0;
}

void FgAirplayServer::log_callback(void* cls, int level, const char* msg)
{
	FgAirplayServer* pServer = (FgAirplayServer*)cls;
	if (!pServer) 
	{
		return;
	}
	if (pServer->m_pCallback)
	{
		pServer->m_pCallback->log(level, msg);
	}
}

static bool IsUsableUnicastAddress(const IP_ADAPTER_UNICAST_ADDRESS* address)
{
	if (address == NULL || address->Address.lpSockaddr == NULL) {
		return false;
	}
	if (address->DadState != IpDadStatePreferred &&
		address->DadState != IpDadStateDeprecated) {
		return false;
	}

	if (address->Address.lpSockaddr->sa_family == AF_INET) {
		const sockaddr_in* ipv4 =
			reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
		return ipv4->sin_addr.s_addr != INADDR_ANY &&
			ipv4->sin_addr.s_addr != INADDR_BROADCAST;
	}

	if (address->Address.lpSockaddr->sa_family == AF_INET6) {
		const sockaddr_in6* ipv6 =
			reinterpret_cast<const sockaddr_in6*>(address->Address.lpSockaddr);
		return !IN6_IS_ADDR_UNSPECIFIED(&ipv6->sin6_addr) &&
			!IN6_IS_ADDR_MULTICAST(&ipv6->sin6_addr);
	}

	return false;
}

static bool GetUsableAddressFamilies(const IP_ADAPTER_ADDRESSES* adapter,
	bool* hasIpv4, bool* hasIpv6)
{
	*hasIpv4 = false;
	*hasIpv6 = false;

	for (const IP_ADAPTER_UNICAST_ADDRESS* address = adapter->FirstUnicastAddress;
		address != NULL; address = address->Next) {
		if (!IsUsableUnicastAddress(address)) {
			continue;
		}

		if (address->Address.lpSockaddr->sa_family == AF_INET) {
			*hasIpv4 = true;
		}
		else if (address->Address.lpSockaddr->sa_family == AF_INET6) {
			*hasIpv6 = true;
		}
	}

	return *hasIpv4 || *hasIpv6;
}

static bool HasUsableGateway(const IP_ADAPTER_ADDRESSES* adapter)
{
	for (const IP_ADAPTER_GATEWAY_ADDRESS_LH* gateway = adapter->FirstGatewayAddress;
		gateway != NULL; gateway = gateway->Next) {
		if (gateway->Address.lpSockaddr == NULL) {
			continue;
		}

		if (gateway->Address.lpSockaddr->sa_family == AF_INET) {
			const sockaddr_in* ipv4 =
				reinterpret_cast<const sockaddr_in*>(gateway->Address.lpSockaddr);
			if (ipv4->sin_addr.s_addr != INADDR_ANY) {
				return true;
			}
		}
		else if (gateway->Address.lpSockaddr->sa_family == AF_INET6) {
			const sockaddr_in6* ipv6 =
				reinterpret_cast<const sockaddr_in6*>(gateway->Address.lpSockaddr);
			if (!IN6_IS_ADDR_UNSPECIFIED(&ipv6->sin6_addr)) {
				return true;
			}
		}
	}

	return false;
}

static ULONG GetAdapterMetric(const IP_ADAPTER_ADDRESSES* adapter,
	bool hasIpv4, bool hasIpv6)
{
	ULONG metric = (std::numeric_limits<ULONG>::max)();
	if (hasIpv4) {
		metric = adapter->Ipv4Metric;
	}
	if (hasIpv6 && adapter->Ipv6Metric < metric) {
		metric = adapter->Ipv6Metric;
	}
	return metric;
}

static BOOL GetPrimaryMacAddress(char strMac[6])
{
	const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS |
		GAA_FLAG_SKIP_ANYCAST |
		GAA_FLAG_SKIP_MULTICAST |
		GAA_FLAG_SKIP_DNS_SERVER;
	ULONG bufferSize = 15 * 1024;
	std::vector<unsigned char> buffer(bufferSize);
	DWORD result = ERROR_BUFFER_OVERFLOW;

	for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
		buffer.resize(bufferSize);
		result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL,
			reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bufferSize);
	}
	if (result != NO_ERROR) {
		return FALSE;
	}

	const IP_ADAPTER_ADDRESSES* bestAdapter = NULL;
	bool bestHasGateway = false;
	ULONG bestMetric = (std::numeric_limits<ULONG>::max)();

	for (const IP_ADAPTER_ADDRESSES* adapter =
		reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
		adapter != NULL; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp ||
			(adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0 ||
			adapter->PhysicalAddressLength != 6 ||
			adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
			adapter->IfType == IF_TYPE_TUNNEL) {
			continue;
		}

		bool hasNonZeroMac = false;
		for (ULONG i = 0; i < adapter->PhysicalAddressLength; ++i) {
			if (adapter->PhysicalAddress[i] != 0) {
				hasNonZeroMac = true;
				break;
			}
		}
		if (!hasNonZeroMac) {
			continue;
		}

		bool hasIpv4;
		bool hasIpv6;
		if (!GetUsableAddressFamilies(adapter, &hasIpv4, &hasIpv6)) {
			continue;
		}

		const bool hasGateway = HasUsableGateway(adapter);
		const ULONG metric = GetAdapterMetric(adapter, hasIpv4, hasIpv6);
		if (bestAdapter == NULL ||
			(hasGateway && !bestHasGateway) ||
			(hasGateway == bestHasGateway && metric < bestMetric)) {
			bestAdapter = adapter;
			bestHasGateway = hasGateway;
			bestMetric = metric;
		}
	}

	if (bestAdapter == NULL) {
		return FALSE;
	}

	memcpy(strMac, bestAdapter->PhysicalAddress, 6);
	return TRUE;
}
