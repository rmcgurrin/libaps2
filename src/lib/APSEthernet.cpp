#include "APSEthernet.h"

#ifdef _WIN32
#include "iphlpapi.h"
#else
#include <ifaddrs.h>
#endif

APSEthernet::APSEthernet() : socket_(ios_, udp::endpoint(udp::v4(), APS_PROTO)) {
    FILE_LOG(logDEBUG) << "Creating ethernet interface";
    //enable broadcasting for enumerating
    socket_.set_option(asio::socket_base::broadcast(true));

    //io_service will return immediately so post receive task before .run()
    setup_receive();

    //Setup the asio service to run on a background thread
    receiveThread_ = std::thread([this](){ ios_.run(); });
};

APSEthernet::~APSEthernet() {
    FILE_LOG(logDEBUG) << "Cleaning up ethernet interface";
    ios_.stop();
    receiveThread_.join();
}

void APSEthernet::setup_receive() {
    socket_.async_receive_from(
        asio::buffer(receivedData_, 2048), senderEndpoint_,
        [this](std::error_code ec, std::size_t bytesReceived)
        {
            //If there is anything to look at hand it off to the sorter
            if (!ec && bytesReceived > 0)
            {
                vector<uint8_t> packetData(receivedData_, receivedData_ + bytesReceived);
                sort_packet(packetData, senderEndpoint_);
            }

            //Start the receiver again
            setup_receive();
    });
}

void APSEthernet::sort_packet(const vector<uint8_t> & packetData, const udp::endpoint & sender) {
    //If we have the endpoint address then add it to the queue
    string senderIP = sender.address().to_string();
    if (msgQueues_.find(senderIP) == msgQueues_.end()) {
        //If it isn't in our list of APSs then perhaps we are seeing an enumerate status response
        //If so add the device info to the set
        if (packetData.size() == 84) {
            devInfo_[senderIP].endpoint = sender;
            //Turn the byte array into a packet to extract the MAC address and firmware version
            //MAC not strictly necessary as we could just use the broadcast MAC address
            APSEthernetPacket packet = APSEthernetPacket(packetData);
            devInfo_[senderIP].macAddr = packet.header.src;
            APSStatusBank_t statusRegs;
            std::copy(packet.payload.begin(), packet.payload.end(), statusRegs.array);
            FILE_LOG(logDEBUG1) << "Added device with IP " << senderIP <<
                " ; MAC addresss " << devInfo_[senderIP].macAddr.to_string() <<
                " ; firmware version " << hexn<4> << statusRegs.userFirmwareVersion;
        }
    }
    else {
        //Turn the byte array into an APSEthernetPacket
        APSEthernetPacket packet = APSEthernetPacket(packetData);
        //Grab a lock and push the packet into the message queue
        mLock_.lock();
        msgQueues_[senderIP].emplace(packet);
        mLock_.unlock();
    }
}

/* PUBLIC methods */

void APSEthernet::init() {
    reset_maps();
}

vector<std::pair<string,string>> APSEthernet::get_local_IPs() {
    vector<std::pair<string,string>> IPs;

    #ifdef _WIN32

    DWORD dwRetVal = 0;
    ULONG family = AF_INET; // we only care about IPv4 for now
    LONG flags = GAA_FLAG_INCLUDE_PREFIX; // not sure what the address prefix is
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    PIP_ADAPTER_ADDRESSES pCurAddresses = nullptr;
    PIP_ADAPTER_UNICAST_ADDRESS pUnicast = nullptr;

    ULONG outBufLen = 15000; //MSDN recommends preallocating 15KB
    pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(outBufLen);
    dwRetVal = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &outBufLen);

    if (dwRetVal == NO_ERROR) {
        for (pCurAddresses = pAddresses; pCurAddresses != nullptr; pCurAddresses = pCurAddresses->Next) {
            FILE_LOG(logDEBUG1) << "Found IPv4 interface.";
            FILE_LOG(logDEBUG3) << "IfIndex (IPv4 interface): " << pCurAddresses->IfIndex;
            FILE_LOG(logDEBUG2) << "Adapter name: " << pCurAddresses->AdapterName;
            FILE_LOG(logDEBUG2) << "Friendly adapter name: " << pCurAddresses->FriendlyName; //TODO figure out unicode printing

            //TODO: since we only support 1GigE should check that here
            FILE_LOG(logDEBUG2) << "Transmit link speed: " << pCurAddresses->TransmitLinkSpeed;
            FILE_LOG(logDEBUG2) << "Receive link speed: " << pCurAddresses->ReceiveLinkSpeed;

            for (pUnicast = pCurAddresses->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
                char IPV4Addr[16]; //should be LPTSTR; 16 is maximum length of null terminated xxx.xxx.xxx.xxx
                DWORD addrSize = 16;
                int val;
                val = WSAAddressToString(pUnicast->Address.lpSockaddr, pUnicast->Address.iSockaddrLength, nullptr, IPV4Addr, &addrSize);
                if ( val != 0) {
                  val = WSAGetLastError();
                  FILE_LOG(logERROR) << "WSAAddressToString error code: " << val;
                  continue;
                }
                //Create a sock_addr for the netmask string prefix to string conversion
                struct sockaddr_in sin;
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_addr.s_addr = htonl(0xffffffff << (32 - pUnicast->OnLinkPrefixLength));

                IPs.push_back(std::pair<string,string>(IPV4Addr, inet_ntoa(sin.sin_addr)));
                FILE_LOG(logDEBUG1) << "IPv4 address: " << IPs.back().first <<
                                        "; Prefix length: " << (unsigned) pUnicast->OnLinkPrefixLength << 
                                        "; Netmask: " << IPs.back().second;
            }
        }
        free(pAddresses);
        pAddresses = nullptr;
    }
    else {
        FILE_LOG(logERROR) << "Call to GetAdaptersAddresses failed.";
        free(pAddresses);
        pAddresses = nullptr;
    }

    #else

    struct ifaddrs *ifap, *ifa;

    getifaddrs(&ifap);
    for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        //We only care about IPv4 addresses
        if ( ifa->ifa_addr == nullptr) continue; //According to Linux Programmers Manual "This field may contain a null pointer."
        if ( ifa->ifa_addr->sa_family == AF_INET ) {
            struct sockaddr_in* sa = (struct sockaddr_in *) ifa->ifa_addr;
            string addr = string(inet_ntoa(sa->sin_addr));
            string netmask = "255.255.255.255";
            if (ifa->ifa_netmask != nullptr) {
                sa = (struct sockaddr_in *) ifa->ifa_netmask;
                netmask = string(inet_ntoa(sa->sin_addr));
            }
            IPs.push_back(std::pair<string,string>(addr, netmask));
            FILE_LOG(logDEBUG1) << "Interface: " << ifa->ifa_name << "; Address: " << IPs.back().first << "; Netmask: " << IPs.back().second;
        }
    }
    freeifaddrs(ifap);
    #endif //_WIN32

    return IPs;
}

set<string> APSEthernet::enumerate() {
	/*
	 * Look for all APS units that respond to the broadcast packet
	 */

	FILE_LOG(logDEBUG1) << "APSEthernet::enumerate";

    reset_maps();

    vector<std::pair<string,string>> localIPs = get_local_IPs();

    for (auto IP : localIPs) {
        //Skip the loop-back
        if ( IP.first.compare("127.0.0.1") == 0 ) continue;

        //Make sure it is a valid IP
        typedef asio::ip::address_v4 addrv4;
        asio::error_code ec;
        addrv4::from_string(IP.first, ec);
        if (ec) {
          FILE_LOG(logERROR) << "Invalid IP address: " << ec.message();
          continue;
        }
        //Put together the broadcast status request
        APSEthernetPacket broadcastPacket = APSEthernetPacket::create_broadcast_packet();
        addrv4 broadcastAddr = addrv4::broadcast(addrv4::from_string(IP.first), addrv4::from_string(IP.second));
        FILE_LOG(logDEBUG1) << "Sending enumerate broadcast out on: " << broadcastAddr.to_string();
        udp::endpoint broadCastEndPoint(broadcastAddr, APS_PROTO);
        socket_.send_to(asio::buffer(broadcastPacket.serialize()), broadCastEndPoint);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    set<string> deviceSerials;
    for (auto kv : devInfo_) {
        FILE_LOG(logINFO) << "Found device: " << kv.first;
        deviceSerials.insert(kv.first);
    }
    return deviceSerials;
}

void APSEthernet::reset_maps() {
    devInfo_.clear();
    msgQueues_.clear();
}

void APSEthernet::connect(string serial) {

    mLock_.lock();
	msgQueues_[serial] = queue<APSEthernetPacket>();
    mLock_.unlock();
    if (devInfo_.find(serial) == devInfo_.end()) {
        devInfo_[serial].endpoint = udp::endpoint(asio::ip::address_v4::from_string(serial), APS_PROTO);
        devInfo_[serial].macAddr = MACAddr("FF:FF:FF:FF:FF:FF");
    }
}

void APSEthernet::disconnect(string serial) {
    mLock_.lock();
	msgQueues_.erase(serial);
    mLock_.unlock();
}

int APSEthernet::send(string serial, APSEthernetPacket msg, bool checkResponse) {
    msg.header.dest = devInfo_[serial].macAddr;
    return send_chunk(serial, vector<APSEthernetPacket>(1, msg), !checkResponse);
}

int APSEthernet::send(string serial, vector<APSEthernetPacket> msg, unsigned ackEvery /* see header for default */) {
    //Fill out the destination  MAC address
    FILE_LOG(logDEBUG3) << "Sending " << msg.size() << " packets to " << serial;
    auto iter = msg.begin();
    bool noACK = false;
    if (ackEvery == 0) {
        noACK = true;
        ackEvery = 1;
    }
    // it's nice to have extra status on slow EPROM writes
    bool verbose = (msg[0].header.command.cmd == static_cast<uint32_t>(APS_COMMANDS::EPROMIO));

    vector<APSEthernetPacket> buffer(ackEvery);

    while (iter != msg.end()) {

        //Copy the next chunk into a buffer
        auto endPoint = iter + ackEvery;
        if (endPoint > msg.end()) {
            endPoint = msg.end();
        }
        auto chunkSize = std::distance(iter, endPoint);
        buffer.resize(chunkSize);
        std::copy(iter, endPoint, buffer.begin());

        for (auto & packet : buffer ) {
            // insert the target MAC address - not really necessary anymore because UDP does filtering
            packet.header.dest = devInfo_[serial].macAddr;
            //NOACK sets the top bit of the command nibble of the command word
            packet.header.command.cmd |= (1 << 3);
        }

        //Apply acknowledge flag to last element of chunk
        if (!noACK) {
            buffer.back().header.command.cmd &= ~(1 << 3);
        }

        auto result = send_chunk(serial, buffer, noACK);
        if (result != 0) {
            return result;
        }
        std::advance(iter, chunkSize);

        if (verbose && (std::distance(msg.begin(), iter) % 1000 == 0)) {
            FILE_LOG(logDEBUG) << "Write " << 100*std::distance(msg.begin(), iter)/msg.size() << "% complete";
        }
    }
    return 0;
}

int APSEthernet::send_chunk(string serial, vector<APSEthernetPacket> chunk, bool noACK) {

    unsigned seqNum, retryct = 0;

    while (retryct++ < 3) {
        seqNum = 0;
        for (auto packet : chunk) {
            packet.header.seqNum = seqNum;
            seqNum++;
            FILE_LOG(logDEBUG4) << "Packet command: " << print_APSCommand(packet.header.command);
            socket_.send_to(asio::buffer(packet.serialize()), devInfo_[serial].endpoint);
            // std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        if (noACK) break;
        //Wait for acknowledge
        //TODO: how to check response mode/stat for success?
        try {
            auto response = receive(serial)[0];
            break;
        }
        catch (std::exception& e) {
            FILE_LOG(logDEBUG) << "No acknowledge received, retrying ...";
        }
    }

    if (retryct == 3) {
        return -1;
    }
    return 0;
}


vector<APSEthernetPacket> APSEthernet::receive(string serial, size_t numPackets, size_t timeoutMS) {
    //Read the packets coming back in up to the timeout
    //Defaults: receive(string serial, size_t numPackets = 1, size_t timeoutMS = 1000);
    std::chrono::time_point<std::chrono::steady_clock> start, end;

    start = std::chrono::steady_clock::now();
    size_t elapsedTime = 0;

    vector<APSEthernetPacket> outVec;

    while (elapsedTime < timeoutMS) {
        if (!msgQueues_[serial].empty()) {
            mLock_.lock();
            outVec.push_back(msgQueues_[serial].front());
            msgQueues_[serial].pop();
            mLock_.unlock();
            FILE_LOG(logDEBUG4) << "Received packet command: " << print_APSCommand(outVec.back().header.command);
            if (outVec.size() == numPackets) {
                FILE_LOG(logDEBUG3) << "Received " << numPackets << " packets from " << serial;
                return outVec;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        end = std::chrono::steady_clock::now();
        elapsedTime =  std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();
    }

    throw APS2_RECEIVE_TIMEOUT;
}
