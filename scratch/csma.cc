/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include <sys/stat.h>    
#include <iomanip> 

#include <map>
#include <vector>
#include <iostream>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("OnePersistentCsmaBroadcastTraced");

// Globals for instrumentation
static std::map<uint64_t, Time> g_txTimeMap;
static std::vector<Time>        g_macDelays;
static uint32_t                 g_packetsSent     = 0;
static uint32_t                 g_packetsReceived = 0;

// Trace‐callback: called at MAC transmit
void MacTxTrace(Ptr<const Packet> packet)
{
  ++g_packetsSent;
  g_txTimeMap[packet->GetUid()] = Simulator::Now();
}

// Trace‐callback: called when PHY transmission ends
void PhyTxEndTrace(Ptr<const Packet> packet)
{
  uint64_t uid = packet->GetUid();
  auto it = g_txTimeMap.find(uid);
  if (it != g_txTimeMap.end())
    {
      g_macDelays.push_back(Simulator::Now() - it->second);
      g_txTimeMap.erase(it);
    }
}

// UDP receive callback: count received packets
void ReceivePacket(Ptr<Socket> socket)
{
  Ptr<Packet> p = socket->Recv();
  ++g_packetsReceived;
}

//----------------------------------------------------------------------------------
// Poisson‐sender: sends one packet, then schedules itself after an exponential delay
void PoissonSend(Ptr<Socket> sock,
                 Ptr<ExponentialRandomVariable> expRv,
                 uint32_t pktSize)
{
  Ptr<Packet> packet = Create<Packet>(pktSize);
  sock->Send(packet);

  // draw next interval
  Time next = Seconds(expRv->GetValue());
  Simulator::Schedule(next,
                      &PoissonSend,
                      sock, expRv, pktSize);
}
//----------------------------------------------------------------------------------

int main(int argc, char *argv[])
{
  uint32_t numNodes   = 5;
  double   simTime    = 100.0;      // seconds
  double   interval   = 0.1;  
  double rate = 10;     // mean inter‐arrival (seconds)
  uint32_t packetSize = 512;      // bytes
  uint16_t rxPort     = 4000;
  std::string out     = "result";

  CommandLine cmd;
  cmd.AddValue("n",        "Number of nodes",             numNodes);
  cmd.AddValue("lambda", "Rate of packets", rate);
  cmd.AddValue("out",      "Run out (unused)",            out);
  cmd.Parse(argc, argv);
    interval=1/rate;
  // 1-persistent CSMA on AdhocWifiMac
  Config::Set(
    "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/"
    "$ns3::AdhocWifiMac/DcaTxop/MinCw",
    UintegerValue(0));
  Config::Set(
    "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/"
    "$ns3::AdhocWifiMac/DcaTxop/MaxCw",
    UintegerValue(0));

  // Create nodes
  NodeContainer nodes;
  nodes.Create(numNodes);

  // Wifi PHY + channel
  YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default();
  YansWifiPhyHelper    wifiPhy;
  wifiPhy.SetChannel(wifiChannel.Create());

  // Ad hoc MAC
  WifiMacHelper wifiMac;
  wifiMac.SetType("ns3::AdhocWifiMac");

  // Constant‐rate manager (802.11a @ 6 Mbps)
  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211a);
  wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                               "DataMode",       StringValue("OfdmRate6Mbps"),
                               "ControlMode",    StringValue("OfdmRate6Mbps"),
                               "NonUnicastMode", StringValue("OfdmRate6Mbps"));

  // Install devices
  NetDeviceContainer devices = wifi.Install(wifiPhy, wifiMac, nodes);

  // Hook traces directly on each device
  for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
      Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(devices.Get(i));

      // MAC‐level
      Ptr<AdhocWifiMac> mac = DynamicCast<AdhocWifiMac>(wifiDev->GetMac());
      mac->TraceConnectWithoutContext("MacTx",
                                     MakeCallback(&MacTxTrace));

      // PHY‐level
      Ptr<WifiPhy> phy = wifiDev->GetPhy();
      phy->TraceConnectWithoutContext("PhyTxEnd",
                                     MakeCallback(&PhyTxEndTrace));
    }

  // Place all nodes at the same point → no hidden terminals
  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(nodes);

  // Internet stack + addressing
  InternetStackHelper internet;
  internet.Install(nodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.0.0", "255.255.0.0");
  Ipv4InterfaceContainer ifs = ipv4.Assign(devices);

  // Broadcast address & port
  Ipv4Address bcastAddr("10.1.255.255");

  // 1) Install UDP **receiver** sockets on each node
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket(nodes.Get(i),
                                            UdpSocketFactory::GetTypeId());
      rx->Bind(InetSocketAddress(Ipv4Address::GetAny(), rxPort));
      rx->SetRecvCallback(MakeCallback(&ReceivePacket));
    }

  // 2) Create UDP **sender** sockets, one per node, set broadcast & connect
  std::vector<Ptr<Socket>> txSockets(numNodes);
  std::vector<Ptr<ExponentialRandomVariable>> expRvs(numNodes);
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      // a) create & bind
      Ptr<Socket> tx = Socket::CreateSocket(nodes.Get(i),
                                            UdpSocketFactory::GetTypeId());
      tx->SetAllowBroadcast(true);
      tx->Bind(InetSocketAddress(ifs.GetAddress(i), 0)); // ephemeral local port

      // b) connect to bcast address + port
      tx->Connect(InetSocketAddress(bcastAddr, rxPort));

      // c) exponential RV for this node
      Ptr<ExponentialRandomVariable> expRv = CreateObject<ExponentialRandomVariable>();
      expRv->SetAttribute("Mean", DoubleValue(interval));

      // store
      txSockets[i] = tx;
      expRvs[i]     = expRv;

      // d) schedule the first send
      Time first = Seconds(expRv->GetValue());
      Simulator::Schedule(first,
                          &PoissonSend,
                          tx, expRv, packetSize);
    }

  // Run simulation
  Simulator::Stop(Seconds(simTime));
  Simulator::Run();
  Simulator::Destroy();

  double mean;
  if (!g_macDelays.empty())
    {
      Time sum = Seconds(0);
      for (auto &d : g_macDelays) sum += d;
      mean = (sum.GetSeconds())/g_macDelays.size();

      std::string path = "./pacn/csma/" + out + ".txt";
    std::cout<<path<<std::endl;
    std::ofstream ofs(path);
    mkdir ("./pacn", 0755);
    mkdir ("./pacn/csma", 0755);
    ofs << g_packetsSent << " "
        << g_packetsReceived << " "
        << std::fixed << std::setprecision(6)<<mean*1e3 << "\n";
    ofs.close();
    }

  return 0;
}
