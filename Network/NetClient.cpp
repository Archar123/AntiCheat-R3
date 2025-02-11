#include "NetClient.hpp"

Error NetClient::Initialize(string ip, uint16_t port)
{
	WSADATA wsaData;
	SOCKET Socket = INVALID_SOCKET;
	SOCKADDR_IN SockAddr;

	this->HardwareID = this->GetHardwareID("C://"); //todo: add some detection to fetch the correct drive automatically

	const char recvBuffer[DEFAULT_RECV_LENGTH] = { 0 };

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		return Error::CANT_STARTUP;
	}

	Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SockAddr.sin_addr.S_un.S_addr = inet_addr(ip.c_str());
	SockAddr.sin_port = htons(port);
	SockAddr.sin_family = AF_INET;

	this->Socket = Socket;

	if (connect(Socket, (SOCKADDR*)(&SockAddr), sizeof(SockAddr)) != 0)
	{
		closesocket(Socket);
		WSACleanup();
		shutdown(Socket, 0);
		return Error::CANT_CONNECT;
	}

	PacketWriter* p = Packets::Builder::ClientHello(this->HardwareID.c_str(), this->GetIpv4().c_str(), this->GetMACAddress().c_str());
	p->WriteString(this->HardwareID.c_str());
	p->WriteString(this->GetIpv4().c_str());

	int BytesSent = send(Socket, (const char*)p->GetBuffer(), p->GetSize(), 0);

	delete p->GetBuffer();
	delete p;

	if (BytesSent <= 0)
	{
		return Error::CANT_SEND;
	}

	int nRecvDataLength;

	if (BytesSent == p->GetSize())
	{
		while ((nRecvDataLength = recv(Socket, (char*)recvBuffer, DEFAULT_RECV_LENGTH, 0)) == 0);

		if (nRecvDataLength > 4 && recvBuffer[0] != 0) //change this later... MINIMUM OFFSETS NEEDED..
		{
			try
			{
				uint16_t packetLength = 0;
				memcpy((void*)&packetLength, (const void*)recvBuffer[0], sizeof(uint16_t));

				PacketWriter* recvP = new PacketWriter(recvBuffer, packetLength); //todo: make constructor where we can pass in a byte* and it auto copies 
				Error err = HandleInboundPacket(recvP); //Handle incoming packet. after the initial handshake is done, this will be called from our recv loop.

				delete recvP->GetBuffer();
				delete recvP;
			}
			catch (std::exception e)
			{
				return Error::DATA_LENGTH_MISMATCH;
			}
		}
	}
	else
	{
		return Error::INCOMPLETE_SEND;
		//error, didnt send enough bytes. maybe packets were modified along the way by the user, or some blip in the wire occured.
	}

	//we can choose to either disconnect or leave the connection open at this point. we still will be sending more data shortly
	//...for now we will leave the connection open

	this->Ip = ip;
	this->Port = port;

	this->ConnectedAt = GetTickCount();
	this->ConnectedDuration = 0;

	//create a thread for handling server replies
	this->RecvLoopThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)&NetClient::ProcessRequests, this, 0, &this->recvThreadId);

	if (this->RecvLoopThread == NULL || this->recvThreadId == NULL)
	{
		return Error::NO_RECV_THREAD;
	}

	return Error::OK;
}

Error NetClient::EndConnection(int reason)
{
	PacketWriter* p = Packets::Builder::ClientGoodbye(reason);
	Error err = Error::OK;

	if (this->Socket != SOCKET_ERROR)
	{
		//send CS_GOODBYE then disconnect

		if (this->SendData(p) != Error::OK)
		{
			err = Error::CANT_SEND;
		}
	}

	if (Socket != SOCKET_ERROR && Socket != NULL)
	{
		closesocket(Socket);
	}

	WSACleanup();
	shutdown(Socket, 0);

	delete p->GetBuffer();
	delete p;
	return err;
}

Error NetClient::SendData(PacketWriter* outPacket)
{
	if (outPacket->GetBuffer() == nullptr || outPacket == nullptr)
		return Error::NULL_MEMORY_REFERENCE;

	Error err = Error::OK;

	const char recvBuffer[DEFAULT_RECV_LENGTH] = { 0 };

	int BytesSent = send(Socket, (const char*)outPacket->GetBuffer(), outPacket->GetSize(), 0);

	int nRecvDataLength = 0;

	if (BytesSent == outPacket->GetSize()) //make sure we sent the hwid
	{
		while ((nRecvDataLength = recv(Socket, (char*)recvBuffer, DEFAULT_RECV_LENGTH, 0)) == 0); //todo: check to see for 'incomplete recvs'  where the recv call does not return all bytes of the packet. usually does not happen on smaller sizes

		if (nRecvDataLength > 0)
		{
			//parse the server response here
			PacketWriter* p_in = new PacketWriter(recvBuffer, nRecvDataLength);

			err = HandleInboundPacket(p_in);

			delete p_in->GetBuffer();
			delete p_in;

			if (err != Error::OK)
			{

			}
		}
	}
	else
	{
		//error, didnt send enough bytes.
		err = Error::INCOMPLETE_SEND;
	}

	return err;
}

void NetClient::ProcessRequests(LPVOID Param)
{
	bool receiving = true;

	NetClient* Client = reinterpret_cast<NetClient*>(Param);

	char recvBuf[DEFAULT_RECV_LENGTH] = { 0 };

	while (receiving)
	{
		SOCKET s = Client->GetClientSocket();

		if (s)
		{
			int bytesIn = recv(s, recvBuf, DEFAULT_RECV_LENGTH, 0);

			if (bytesIn != SOCKET_ERROR)
			{
				PacketWriter* p = new PacketWriter(recvBuf, bytesIn);
				Client->HandleInboundPacket(p);
			}
		}
	}
}

string NetClient::GetIpv4() //todo: finish this function: convert values into string 
{
	struct IPv4
	{
		unsigned char b1, b2, b3, b4;
	};

	IPv4 myIP;
	string sIpv4;

	char szBuffer[1024];

#ifdef WIN32
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 0);
	if (::WSAStartup(wVersionRequested, &wsaData) != 0)
		return "";
#endif
	if (gethostname(szBuffer, sizeof(szBuffer)) == SOCKET_ERROR)
	{
#ifdef WIN32
		WSACleanup();
#endif
		return "";
	}

	struct hostent* host = gethostbyname(szBuffer);
	if (host == NULL)
	{
#ifdef WIN32
		WSACleanup();
#endif
		return "";
	}

	//Obtain the computer's IP
	myIP.b1 = ((struct in_addr*)(host->h_addr))->S_un.S_un_b.s_b1;
	myIP.b2 = ((struct in_addr*)(host->h_addr))->S_un.S_un_b.s_b2;
	myIP.b3 = ((struct in_addr*)(host->h_addr))->S_un.S_un_b.s_b3;
	myIP.b4 = ((struct in_addr*)(host->h_addr))->S_un.S_un_b.s_b4;

#ifdef WIN32
	WSACleanup();
#endif

	char b1[5], b2[5], b3[5], b4[5];

	itoa(myIP.b1, b1, 10);
	itoa(myIP.b2, b2, 10);
	itoa(myIP.b3, b3, 10);
	itoa(myIP.b4, b4, 10);

	sIpv4 = b1;
	sIpv4 = sIpv4 + "." + b2 + "." + b3 + "." + b4;

	return sIpv4;
}

string NetClient::GetMACAddress()
{
	PIP_ADAPTER_INFO AdapterInfo;
	DWORD dwBufLen = sizeof(IP_ADAPTER_INFO);
	char* mac_addr = (char*)malloc(18);

	AdapterInfo = (IP_ADAPTER_INFO*)malloc(sizeof(IP_ADAPTER_INFO));
	if (AdapterInfo == NULL)
	{
		printf("Error allocating memory needed to call GetAdaptersinfo\n");
		free(mac_addr);
		return NULL; // it is safe to call free(NULL)
	}

	// Make an initial call to GetAdaptersInfo to get the necessary size into the dwBufLen variable
	if (GetAdaptersInfo(AdapterInfo, &dwBufLen) == ERROR_BUFFER_OVERFLOW) {
		free(AdapterInfo);
		AdapterInfo = (IP_ADAPTER_INFO*)malloc(dwBufLen);
		if (AdapterInfo == NULL) {
			printf("Error allocating memory needed to call GetAdaptersinfo\n");
			free(mac_addr);
			return NULL;
		}
	}

	if (GetAdaptersInfo(AdapterInfo, &dwBufLen) == NO_ERROR) {
		// Contains pointer to current adapter info
		PIP_ADAPTER_INFO pAdapterInfo = AdapterInfo;
		do {
			// technically should look at pAdapterInfo->AddressLength
			//   and not assume it is 6.
			sprintf(mac_addr, "%02X:%02X:%02X:%02X:%02X:%02X",
				pAdapterInfo->Address[0], pAdapterInfo->Address[1],
				pAdapterInfo->Address[2], pAdapterInfo->Address[3],
				pAdapterInfo->Address[4], pAdapterInfo->Address[5]);
			printf("Address: %s, mac: %s\n", pAdapterInfo->IpAddressList.IpAddress.String, mac_addr);
			// print them all, return the last one.
			// return mac_addr;

			printf("\n");
			pAdapterInfo = pAdapterInfo->Next;
		} while (pAdapterInfo);
	}
	free(AdapterInfo);
	return mac_addr; // caller must free.
}

string NetClient::GetHardwareID(string driveRoot)
{
	string HWID;

	CHAR volumeName[MAX_PATH + 1] = { 0 };
	CHAR fileSystemName[MAX_PATH + 1] = { 0 };
	DWORD serialNumber = 0;
	DWORD maxComponentLen = 0;
	DWORD fileSystemFlags = 0;

	if (!GetVolumeInformationA(driveRoot.c_str(), volumeName, ARRAYSIZE(volumeName), &serialNumber, &maxComponentLen, &fileSystemFlags, fileSystemName, ARRAYSIZE(fileSystemName)))
	{
		return (string)NULL;
	}

	CHAR serialBuf[20];
	_itoa_s(serialNumber, serialBuf, 10);

	CHAR username[1024];
	DWORD size = 1024;
	GetUserNameA((CHAR*)username, &size);

	HWID = username;
	HWID += "-";
	HWID += serialBuf;
	HWID += '\0';

	return HWID;
}

Error NetClient::HandleInboundPacket(PacketWriter* p)
{
	if (p->GetBuffer() == NULL || p == nullptr)
		return Error::NULL_MEMORY_REFERENCE;

	Error err;

	uint16_t opcode = 0;
	const unsigned char* buff = p->GetBuffer();

	memcpy((void*)&opcode, buff, sizeof(uint16_t));

	switch (opcode) //parse server-to-client packets
	{
	case Packets::Opcodes::SC_HELLO:
		break;

	case Packets::Opcodes::SC_HEARTBEAT:
		break;

	default:
		err = Error::BAD_OPCODE;
		break;
	}

	return err;
}

uint64_t NetClient::MakeHashFromServerResponse(PacketWriter* p)
{
	return 0;
}
