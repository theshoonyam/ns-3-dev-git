/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include <fstream>
#include <iomanip>
#include <map>
#include <queue>
#include <vector>
#include <sys/stat.h>    


#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/adhoc-aloha-noack-ideal-phy-helper.h"
#include "ns3/single-model-spectrum-channel.h"
#include "ns3/propagation-delay-model.h"
#include "ns3/random-variable-stream.h"
#include "ns3/wifi-spectrum-value-helper.h"
#include "ns3/udp-socket-factory.h"

using namespace ns3;

/* ----- user-tunable defaults ----------------------------------- */
static const Time      kSlot  = MicroSeconds (700);  // slot length. Some extra time to account for processing delays
static const uint32_t  kPktSz = 512;                  // payload bytes
static double          g_p    = 1.0;                 // P(tx | backlogged)

/* ----- global stats ------------------------------------------- */
static std::map<uint64_t, Time> g_macTx;
static std::vector<double>      g_delay;
static uint64_t                 g_sent = 0;
static uint64_t                 g_recv = 0;

/* ----- trace callbacks ---------------------------------------- */
static void MacTxCb (Ptr<const Packet> p)
{
  g_macTx[p->GetUid ()] = Simulator::Now ();
  ++g_sent;
}

static void PhyTxEndCb (Ptr<const Packet> p)
{
  auto it = g_macTx.find (p->GetUid ());
  if (it == g_macTx.end ()) return;
  g_delay.push_back ( (Simulator::Now () - it->second).GetSeconds () );
  g_macTx.erase (it);
}
static void RecvCb (Ptr<Socket> s)
{
  while (s->Recv ()) ++g_recv;
}

/* ----- RNG helpers -------------------------------------------- */
static Ptr<UniformRandomVariable>     urv = CreateObject<UniformRandomVariable>();
static Ptr<ExponentialRandomVariable> erv = CreateObject<ExponentialRandomVariable>();

/* ----- per-node state ----------------------------------------- */
struct NodeState
{
  std::queue<Ptr<Packet>> q;
  Ptr<Socket>             txSock;
};
static void ScheduleArrival (NodeState*, double);

static void Generate (NodeState* st, double lambda)
{
  st->q.push (Create<Packet> (kPktSz));
  ScheduleArrival (st, lambda);
}
static void ScheduleArrival (NodeState* st, double lambda)
{
  if (lambda <= 0) return;
  Simulator::Schedule ( Seconds (erv->GetValue ()), &Generate, st, lambda );
}

/* ----- slot timer --------------------------------------------- */
static void SlotTick (std::vector<NodeState>* vec)
{
  for (auto& st : *vec)
    {
      if (st.q.empty ()) continue;
      if (urv->GetValue () < g_p)
        {
          st.txSock->Send (st.q.front ());   // broadcast UDP datagram
          st.q.pop ();
        }
    }
  Simulator::Schedule (kSlot, &SlotTick, vec);
}

/* ===========================  main  =========================== */
int main (int argc, char* argv[])
{
  uint32_t n = 5;
  double   lambda = 1000.0;       // pkts/s per node
  double   dur    = 100.0;       // simulation time
  std::string outTag = "results";

  CommandLine cmd;
  cmd.AddValue ("n",      "number of nodes",          n);
  cmd.AddValue ("lambda", "Poisson arrival rate",     lambda);
//   cmd.AddValue ("time",   "simulation duration (s)",  dur);
//   cmd.AddValue ("p",      "slot transmit probability",g_p);
    cmd.AddValue("out",    "output filename tag",       outTag);

    cmd.Parse (argc, argv);

  /* ---- MAC rate so frame fits in slot ------------------------ */
//   Config::SetDefault ("ns3::AlohaNoackMac::DataRate", StringValue ("10Mbps"));

  /* ---- Spectrum channel with constant delay ------------------ */
  Ptr<SpectrumChannel> chan = CreateObject<SingleModelSpectrumChannel>();
  chan->SetPropagationDelayModel (CreateObject<ConstantSpeedPropagationDelayModel>());

  WifiSpectrumValue5MhzFactory sf;
  Ptr<SpectrumValue> txPsd   = sf.CreateTxPowerSpectralDensity (0.1, 1);
  Ptr<SpectrumValue> noisePsd= sf.CreateConstant (1e-15);

  AdhocAlohaNoackIdealPhyHelper helper;
  helper.SetChannel (chan);
  helper.SetTxPowerSpectralDensity   (txPsd);
  helper.SetNoisePowerSpectralDensity(noisePsd);
  helper.SetPhyAttribute (
    "Rate",
    DataRateValue (DataRate ("6Mbps"))
);

  /* ---- Nodes, devices, mobility ------------------------------ */
  NodeContainer nodes; nodes.Create (n);
  NetDeviceContainer devs = helper.Install (nodes);

  MobilityHelper mob;
  mob.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mob.Install (nodes);

  /* ---- Internet stack + /24 subnet --------------------------- */
  InternetStackHelper internet;
  internet.Install (nodes);
  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.0.0", "255.255.0.0");
  Ipv4InterfaceContainer ifs = ipv4.Assign (devs);

  /* ---- UDP broadcast sockets --------------------------------- */
  const uint16_t port = 9999;
  std::vector<NodeState> state (n);

  for (uint32_t i = 0; i < n; ++i)
    {
      /* TX socket */
      Ptr<Socket> tx =
          Socket::CreateSocket (nodes.Get(i), UdpSocketFactory::GetTypeId ());
      tx->SetAllowBroadcast (true);
      InetSocketAddress bcast ("10.1.255.255", port);
      tx->Connect (bcast);
      state[i].txSock = tx;

      /* RX socket (bind to any addr on port) */
      Ptr<Socket> rx =
          Socket::CreateSocket (nodes.Get(i), UdpSocketFactory::GetTypeId ());
      InetSocketAddress local (Ipv4Address::GetAny (), port);
      rx->Bind (local);
      rx->SetRecvCallback (MakeCallback (&RecvCb));
    }

  /* ---- Connect traces ---------------------------------------- */
  Config::ConnectWithoutContext ("/NodeList/*/DeviceList/*/MacTx",    MakeCallback (&MacTxCb));
  Config::ConnectWithoutContext ("/NodeList/*/DeviceList/*/Phy/TxEnd",MakeCallback (&PhyTxEndCb));

  /* ---- Traffic generation ------------------------------------ */
  erv->SetAttribute ("Mean", DoubleValue (1.0 / lambda));
  for (auto& s : state) ScheduleArrival (&s, lambda);
  Simulator::Schedule (kSlot, &SlotTick, &state);

  Simulator::Stop (Seconds (dur));
  Simulator::Run ();
  Simulator::Destroy ();

  /* ---- store delays + summary -------------------------------- */
  std::ofstream out ("mac_delays.txt");
  out.setf (std::ios::fixed); out << std::setprecision (6);
  for (double d : g_delay) out << d << '\n';
  out.close ();

  double mean = 0.0;
  if (!g_delay.empty ())
    {
      for (double d : g_delay) mean += d;
      mean /= g_delay.size ();
    }
  std::cout.setf (std::ios::fixed);
  mkdir("./pacn", 0755);
  mkdir("./pacn/slotted_aloha", 0755);

  std::string path = "./pacn/slotted_aloha/" + outTag + ".txt";
  std::cout<<path<<std::endl;
  std::ofstream ofs(path);

  ofs << g_sent << " "
      << g_recv << " "
      << std::fixed << std::setprecision(6) << mean*1e3 << "\n";
  ofs.close();
  return 0;
}
