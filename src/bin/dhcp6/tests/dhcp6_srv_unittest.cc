// Copyright (C) 2011-2013  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <config.h>

#include <asiolink/io_address.h>
#include <config/ccsession.h>
#include <dhcp/dhcp6.h>
#include <dhcp/duid.h>
#include <dhcp/option.h>
#include <dhcp/option_custom.h>
#include <dhcp/option6_addrlst.h>
#include <dhcp/option6_ia.h>
#include <dhcp/option6_iaaddr.h>
#include <dhcp/option_int_array.h>
#include <dhcp/iface_mgr.h>
#include <dhcp6/config_parser.h>
#include <dhcp6/dhcp6_srv.h>
#include <dhcp/dhcp6.h>
#include <dhcpsrv/cfgmgr.h>
#include <dhcpsrv/lease_mgr.h>
#include <dhcpsrv/lease_mgr_factory.h>
#include <dhcpsrv/utils.h>
#include <util/buffer.h>
#include <util/range_utilities.h>

#include <hooks/server_hooks.h>
#include <hooks/hooks_manager.h>

#include <boost/scoped_ptr.hpp>
#include <gtest/gtest.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <list>

using namespace isc;
using namespace isc::asiolink;
using namespace isc::config;
using namespace isc::data;
using namespace isc::dhcp;
using namespace isc::util;
using namespace isc::hooks;
using namespace std;

// namespace has to be named, because friends are defined in Dhcpv6Srv class
// Maybe it should be isc::test?
namespace {

class NakedDhcpv6Srv: public Dhcpv6Srv {
    // "naked" Interface Manager, exposes internal members
public:
    NakedDhcpv6Srv(uint16_t port) : Dhcpv6Srv(port) {
        // Open the "memfile" database for leases
        std::string memfile = "type=memfile";
        LeaseMgrFactory::create(memfile);
    }

    /// @brief fakes packet reception
    /// @param timeout ignored
    ///
    /// The method receives all packets queued in receive
    /// queue, one after another. Once the queue is empty,
    /// it initiates the shutdown procedure.
    ///
    /// See fake_received_ field for description
    virtual Pkt6Ptr receivePacket(int /*timeout*/) {

        // If there is anything prepared as fake incoming
        // traffic, use it
        if (!fake_received_.empty()) {
            Pkt6Ptr pkt = fake_received_.front();
            fake_received_.pop_front();
            return (pkt);
        }

        // If not, just trigger shutdown and
        // return immediately
        shutdown();
        return (Pkt6Ptr());
    }

    /// @brief fake packet sending
    ///
    /// Pretend to send a packet, but instead just store
    /// it in fake_send_ list where test can later inspect
    /// server's response.
    virtual void sendPacket(const Pkt6Ptr& pkt) {
        fake_sent_.push_back(pkt);
    }

    /// @brief adds a packet to fake receive queue
    ///
    /// See fake_received_ field for description
    void fakeReceive(const Pkt6Ptr& pkt) {
        fake_received_.push_back(pkt);
    }

    virtual ~NakedDhcpv6Srv() {
        // Remove all registered hook points (it must be done even for tests that
        // do not use hooks as the base class - Dhcpv6Srv registers hooks)
        ServerHooks::getServerHooks().reset();

        // Close the lease database
        LeaseMgrFactory::destroy();
    }

    using Dhcpv6Srv::processSolicit;
    using Dhcpv6Srv::processRequest;
    using Dhcpv6Srv::processRenew;
    using Dhcpv6Srv::processRelease;
    using Dhcpv6Srv::createStatusCode;
    using Dhcpv6Srv::selectSubnet;
    using Dhcpv6Srv::sanityCheck;
    using Dhcpv6Srv::loadServerID;
    using Dhcpv6Srv::writeServerID;

    /// @brief packets we pretend to receive
    ///
    /// Instead of setting up sockets on interfaces that change between OSes, it
    /// is much easier to fake packet reception. This is a list of packets that
    /// we pretend to have received. You can schedule new packets to be received
    /// using fakeReceive() and NakedDhcpv6Srv::receivePacket() methods.
    list<Pkt6Ptr> fake_received_;

    list<Pkt6Ptr> fake_sent_;
};

static const char* DUID_FILE = "server-id-test.txt";

// test fixture for any tests requiring blank/empty configuration
// serves as base class for additional tests
class NakedDhcpv6SrvTest : public ::testing::Test {
public:

    NakedDhcpv6SrvTest() : rcode_(-1) {
        // it's ok if that fails. There should not be such a file anyway
        unlink(DUID_FILE);

        const IfaceMgr::IfaceCollection& ifaces = IfaceMgr::instance().getIfaces();

        // There must be some interface detected
        if (ifaces.empty()) {
            // We can't use ASSERT in constructor
            ADD_FAILURE() << "No interfaces detected.";
        }

        valid_iface_ = ifaces.begin()->getName();
    }

    // Generate IA_NA option with specified parameters
    boost::shared_ptr<Option6IA> generateIA(uint32_t iaid, uint32_t t1, uint32_t t2) {
        boost::shared_ptr<Option6IA> ia =
            boost::shared_ptr<Option6IA>(new Option6IA(D6O_IA_NA, iaid));
        ia->setT1(t1);
        ia->setT2(t2);
        return (ia);
    }

    /// @brief generates interface-id option, based on text
    ///
    /// @param iface_id textual representation of the interface-id content
    ///
    /// @return pointer to the option object
    OptionPtr generateInterfaceId(const string& iface_id) {
        OptionBuffer tmp(iface_id.begin(), iface_id.end());
        return OptionPtr(new Option(Option::V6, D6O_INTERFACE_ID, tmp));
    }

    // Generate client-id option
    OptionPtr generateClientId(size_t duid_size = 32) {

        OptionBuffer clnt_duid(duid_size);
        for (int i = 0; i < duid_size; i++) {
            clnt_duid[i] = 100 + i;
        }

        duid_ = DuidPtr(new DUID(clnt_duid));

        return (OptionPtr(new Option(Option::V6, D6O_CLIENTID,
                                     clnt_duid.begin(),
                                     clnt_duid.begin() + duid_size)));
    }

    // Checks if server response (ADVERTISE or REPLY) includes proper server-id.
    void checkServerId(const Pkt6Ptr& rsp, const OptionPtr& expected_srvid) {
        // check that server included its server-id
        OptionPtr tmp = rsp->getOption(D6O_SERVERID);
        EXPECT_EQ(tmp->getType(), expected_srvid->getType() );
        ASSERT_EQ(tmp->len(), expected_srvid->len() );
        EXPECT_TRUE(tmp->getData() == expected_srvid->getData());
    }

    // Checks if server response (ADVERTISE or REPLY) includes proper client-id.
    void checkClientId(const Pkt6Ptr& rsp, const OptionPtr& expected_clientid) {
        // check that server included our own client-id
        OptionPtr tmp = rsp->getOption(D6O_CLIENTID);
        ASSERT_TRUE(tmp);
        EXPECT_EQ(expected_clientid->getType(), tmp->getType());
        ASSERT_EQ(expected_clientid->len(), tmp->len());

        // check that returned client-id is valid
        EXPECT_TRUE(expected_clientid->getData() == tmp->getData());
    }

    // Checks if server response is a NAK
    void checkNakResponse(const Pkt6Ptr& rsp, uint8_t expected_message_type,
                          uint32_t expected_transid,
                          uint16_t expected_status_code) {
        // Check if we get response at all
        checkResponse(rsp, expected_message_type, expected_transid);

        // Check that IA_NA was returned
        OptionPtr option_ia_na = rsp->getOption(D6O_IA_NA);
        ASSERT_TRUE(option_ia_na);

        // check that the status is no address available
        boost::shared_ptr<Option6IA> ia = boost::dynamic_pointer_cast<Option6IA>(option_ia_na);
        ASSERT_TRUE(ia);

        checkIA_NAStatusCode(ia, expected_status_code);
    }

    // Checks that server rejected IA_NA, i.e. that it has no addresses and
    // that expected status code really appears there. In some limited cases
    // (reply to RELEASE) it may be used to verify positive case, where
    // IA_NA response is expected to not include address.
    //
    // Status code indicates type of error encountered (in theory it can also
    // indicate success, but servers typically don't send success status
    // as this is the default result and it saves bandwidth)
    void checkIA_NAStatusCode(const boost::shared_ptr<Option6IA>& ia,
                            uint16_t expected_status_code) {
        // Make sure there is no address assigned.
        EXPECT_FALSE(ia->getOption(D6O_IAADDR));

        // T1, T2 should be zeroed
        EXPECT_EQ(0, ia->getT1());
        EXPECT_EQ(0, ia->getT2());

        OptionCustomPtr status =
            boost::dynamic_pointer_cast<OptionCustom>(ia->getOption(D6O_STATUS_CODE));

        // It is ok to not include status success as this is the default behavior
        if (expected_status_code == STATUS_Success && !status) {
            return;
        }

        EXPECT_TRUE(status);

        if (status) {
            // We don't have dedicated class for status code, so let's just interpret
            // first 2 bytes as status. Remainder of the status code option content is
            // just a text explanation what went wrong.
            EXPECT_EQ(static_cast<uint16_t>(expected_status_code),
                      status->readInteger<uint16_t>(0));
        }
    }

    void checkMsgStatusCode(const Pkt6Ptr& msg, uint16_t expected_status) {
        OptionCustomPtr status =
            boost::dynamic_pointer_cast<OptionCustom>(msg->getOption(D6O_STATUS_CODE));

        // It is ok to not include status success as this is the default behavior
        if (expected_status == STATUS_Success && !status) {
            return;
        }

        EXPECT_TRUE(status);
        if (status) {
            // We don't have dedicated class for status code, so let's just interpret
            // first 2 bytes as status. Remainder of the status code option content is
            // just a text explanation what went wrong.
            EXPECT_EQ(static_cast<uint16_t>(expected_status),
                      status->readInteger<uint16_t>(0));
        }
    }

    // Basic checks for generated response (message type and transaction-id).
    void checkResponse(const Pkt6Ptr& rsp, uint8_t expected_message_type,
                       uint32_t expected_transid) {
        ASSERT_TRUE(rsp);
        EXPECT_EQ(expected_message_type, rsp->getType());
        EXPECT_EQ(expected_transid, rsp->getTransid());
    }

    virtual ~NakedDhcpv6SrvTest() {
        // Let's clean up if there is such a file.
        unlink(DUID_FILE);
    };

    // A DUID used in most tests (typically as client-id)
    DuidPtr duid_;

    int rcode_;
    ConstElementPtr comment_;

    // Name of a valid network interface
    string valid_iface_;
};

// Provides suport for tests against a preconfigured subnet6
// extends upon NakedDhcp6SrvTest
class Dhcpv6SrvTest : public NakedDhcpv6SrvTest {
public:
    /// Name of the server-id file (used in server-id tests)

    // these are empty for now, but let's keep them around
    Dhcpv6SrvTest() {
        subnet_ = Subnet6Ptr(new Subnet6(IOAddress("2001:db8:1::"), 48, 1000,
                                         2000, 3000, 4000));
        pool_ = Pool6Ptr(new Pool6(Pool6::TYPE_IA, IOAddress("2001:db8:1:1::"), 64));
        subnet_->addPool(pool_);

        CfgMgr::instance().deleteSubnets6();
        CfgMgr::instance().addSubnet6(subnet_);
    }

    // Checks that server response (ADVERTISE or REPLY) contains proper IA_NA option
    // It returns IAADDR option for each chaining with checkIAAddr method.
    boost::shared_ptr<Option6IAAddr> checkIA_NA(const Pkt6Ptr& rsp, uint32_t expected_iaid,
                                            uint32_t expected_t1, uint32_t expected_t2) {
        OptionPtr tmp = rsp->getOption(D6O_IA_NA);
        // Can't use ASSERT_TRUE() in method that returns something
        if (!tmp) {
            ADD_FAILURE() << "IA_NA option not present in response";
            return (boost::shared_ptr<Option6IAAddr>());
        }

        boost::shared_ptr<Option6IA> ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
        if (!ia) {
            ADD_FAILURE() << "IA_NA cannot convert option ptr to Option6";
            return (boost::shared_ptr<Option6IAAddr>());
        }

        EXPECT_EQ(expected_iaid, ia->getIAID());
        EXPECT_EQ(expected_t1, ia->getT1());
        EXPECT_EQ(expected_t2, ia->getT2());

        tmp = ia->getOption(D6O_IAADDR);
        boost::shared_ptr<Option6IAAddr> addr = boost::dynamic_pointer_cast<Option6IAAddr>(tmp);
        return (addr);
    }

    // Check that generated IAADDR option contains expected address
    // and lifetime values match the configured subnet
    void checkIAAddr(const boost::shared_ptr<Option6IAAddr>& addr,
                     const IOAddress& expected_addr,
                     uint32_t /* expected_preferred */,
                     uint32_t /* expected_valid */) {

        // Check that the assigned address is indeed from the configured pool.
        // Note that when comparing addresses, we compare the textual
        // representation. IOAddress does not support being streamed to
        // an ostream, which means it can't be used in EXPECT_EQ.
        EXPECT_TRUE(subnet_->inPool(addr->getAddress()));
        EXPECT_EQ(expected_addr.toText(), addr->getAddress().toText());
        EXPECT_EQ(addr->getPreferred(), subnet_->getPreferred());
        EXPECT_EQ(addr->getValid(), subnet_->getValid());
    }

    // Checks if the lease sent to client is present in the database
    // and is valid when checked agasint the configured subnet
    Lease6Ptr checkLease(const DuidPtr& duid, const OptionPtr& ia_na,
                         boost::shared_ptr<Option6IAAddr> addr) {
        boost::shared_ptr<Option6IA> ia = boost::dynamic_pointer_cast<Option6IA>(ia_na);

        Lease6Ptr lease = LeaseMgrFactory::instance().getLease6(addr->getAddress());
        if (!lease) {
            cout << "Lease for " << addr->getAddress().toText()
                 << " not found in the database backend.";
            return (Lease6Ptr());
        }

        EXPECT_EQ(addr->getAddress().toText(), lease->addr_.toText());
        EXPECT_TRUE(*lease->duid_ == *duid);
        EXPECT_EQ(ia->getIAID(), lease->iaid_);
        EXPECT_EQ(subnet_->getID(), lease->subnet_id_);

        return (lease);
    }

    ~Dhcpv6SrvTest() {
        CfgMgr::instance().deleteSubnets6();
    };

    // A subnet used in most tests
    Subnet6Ptr subnet_;

    // A pool used in most tests
    Pool6Ptr pool_;
};

// This test verifies that incoming SOLICIT can be handled properly when
// there are no subnets configured.
//
// This test sends a SOLICIT and the expected response
// is an ADVERTISE with STATUS_NoAddrsAvail and no address provided in the
// response
TEST_F(NakedDhcpv6SrvTest, SolicitNoSubnet) {
    NakedDhcpv6Srv srv(0);

    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    sol->addOption(generateIA(234, 1500, 3000));
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr reply = srv.processSolicit(sol);

    // check that we get the right NAK
    checkNakResponse (reply, DHCPV6_ADVERTISE, 1234, STATUS_NoAddrsAvail);
}

// This test verifies that incoming REQUEST can be handled properly when
// there are no subnets configured.
//
// This test sends a REQUEST and the expected response
// is an REPLY with STATUS_NoAddrsAvail and no address provided in the
// response
TEST_F(NakedDhcpv6SrvTest, RequestNoSubnet) {
    NakedDhcpv6Srv srv(0);

    // Let's create a REQUEST
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_REQUEST, 1234));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(234, 1500, 3000);

    // with a hint
    IOAddress hint("2001:db8:1:1::dead:beef");
    OptionPtr hint_opt(new Option6IAAddr(D6O_IAADDR, hint, 300, 500));
    ia->addOption(hint_opt);
    req->addOption(ia);
    OptionPtr clientid = generateClientId();
    req->addOption(clientid);

    // server-id is mandatory in REQUEST
    req->addOption(srv.getServerID());

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRequest(req);

    // check that we get the right NAK
    checkNakResponse (reply, DHCPV6_REPLY, 1234, STATUS_NoAddrsAvail);
}

// This test verifies that incoming RENEW can be handled properly, even when
// no subnets are configured.
//
// This test sends a RENEW and the expected response
// is an REPLY with STATUS_NoBinding and no address provided in the
// response
TEST_F(NakedDhcpv6SrvTest, RenewNoSubnet) {
    NakedDhcpv6Srv srv(0);

    const IOAddress addr("2001:db8:1:1::cafe:babe");
    const uint32_t iaid = 234;

    // Generate client-id also duid_
    OptionPtr clientid = generateClientId();

    // Let's create a RENEW
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_RENEW, 1234));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(iaid, 1500, 3000);

    OptionPtr renewed_addr_opt(new Option6IAAddr(D6O_IAADDR, addr, 300, 500));
    ia->addOption(renewed_addr_opt);
    req->addOption(ia);
    req->addOption(clientid);

    // Server-id is mandatory in RENEW
    req->addOption(srv.getServerID());

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRenew(req);

    // check that we get the right NAK
    checkNakResponse (reply, DHCPV6_REPLY, 1234, STATUS_NoBinding);
}

// This test verifies that incoming RELEASE can be handled properly, even when
// no subnets are configured.
//
// This test sends a RELEASE and the expected response
// is an REPLY with STATUS_NoBinding and no address provided in the
// response
TEST_F(NakedDhcpv6SrvTest, ReleaseNoSubnet) {
    NakedDhcpv6Srv srv(0);

    const IOAddress addr("2001:db8:1:1::cafe:babe");
    const uint32_t iaid = 234;

    // Generate client-id also duid_
    OptionPtr clientid = generateClientId();

    // Let's create a RELEASE
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_RELEASE, 1234));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(iaid, 1500, 3000);

    OptionPtr released_addr_opt(new Option6IAAddr(D6O_IAADDR, addr, 300, 500));
    ia->addOption(released_addr_opt);
    req->addOption(ia);
    req->addOption(clientid);

    // Server-id is mandatory in RELEASE
    req->addOption(srv.getServerID());

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRelease(req);

    // check that we get the right NAK
    checkNakResponse (reply, DHCPV6_REPLY, 1234, STATUS_NoBinding);
}


// Test verifies that the Dhcpv6_srv class can be instantiated. It checks a mode
// without open sockets and with sockets opened on a high port (to not require
// root privileges).
TEST_F(Dhcpv6SrvTest, basic) {
    // srv has stubbed interface detection. It will read
    // interfaces.txt instead. It will pretend to have detected
    // fe80::1234 link-local address on eth0 interface. Obviously
    // an attempt to bind this socket will fail.
    boost::scoped_ptr<Dhcpv6Srv> srv;

    ASSERT_NO_THROW( {
        // Skip opening any sockets
        srv.reset(new Dhcpv6Srv(0));
    });
    srv.reset();
    ASSERT_NO_THROW({
        // open an unpriviledged port
        srv.reset(new Dhcpv6Srv(DHCP6_SERVER_PORT + 10000));
    });
}

// Test checks that DUID is generated properly
TEST_F(Dhcpv6SrvTest, DUID) {

    boost::scoped_ptr<Dhcpv6Srv> srv;
    ASSERT_NO_THROW( {
        srv.reset(new NakedDhcpv6Srv(0));
    });

    OptionPtr srvid = srv->getServerID();
    ASSERT_TRUE(srvid);

    EXPECT_EQ(D6O_SERVERID, srvid->getType());

    OutputBuffer buf(32);
    srvid->pack(buf);

    // length of the actual DUID
    size_t len = buf.getLength() - srvid->getHeaderLen();

    InputBuffer data(buf.getData(), buf.getLength());

    // ignore first four bytes (standard DHCPv6 header)
    data.readUint32();

    uint16_t duid_type = data.readUint16();
    cout << "Duid-type=" << duid_type << endl;
    switch(duid_type) {
    case DUID::DUID_LLT: {
        // DUID must contain at least 6 bytes long MAC
        // + 8 bytes of fixed header
        EXPECT_GE(14, len);

        uint16_t hw_type = data.readUint16();
        // there's no real way to find out "correct"
        // hardware type
        EXPECT_GT(hw_type, 0);

        // check that timer is counted since 1.1.2000,
        // not from 1.1.1970.
        uint32_t seconds = data.readUint32();
        EXPECT_LE(seconds, DUID_TIME_EPOCH);
        // this test will start failing after 2030.
        // Hopefully we will be at BIND12 by then.

        // MAC must not be zeros
        vector<uint8_t> mac(len-8);
        vector<uint8_t> zeros(len-8, 0);
        data.readVector(mac, len-8);
        EXPECT_TRUE(mac != zeros);
        break;
    }
    case DUID::DUID_EN: {
        // there's not much we can check. Just simple
        // check if it is not all zeros
        vector<uint8_t> content(len-2);
        data.readVector(content, len-2);
        EXPECT_FALSE(isRangeZero(content.begin(), content.end()));
        break;
    }
    case DUID::DUID_LL: {
        // not supported yet
        cout << "Test not implemented for DUID-LL." << endl;

        // No failure here. There's really no way for test LL DUID. It doesn't
        // even make sense to check if that Link Layer is actually present on
        // a physical interface. RFC3315 says a server should write its DUID
        // and keep it despite hardware changes.
        break;
    }
    case DUID::DUID_UUID: // not supported yet
    default:
        ADD_FAILURE() << "Not supported duid type=" << duid_type << endl;
        break;
    }
}

// This test checks if Option Request Option (ORO) is parsed correctly
// and the requested options are actually assigned.
TEST_F(Dhcpv6SrvTest, advertiseOptions) {
    ConstElementPtr x;
    string config = "{ \"interface\": [ \"all\" ],"
        "\"preferred-lifetime\": 3000,"
        "\"rebind-timer\": 2000, "
        "\"renew-timer\": 1000, "
        "\"subnet6\": [ { "
        "    \"pool\": [ \"2001:db8:1::/64\" ],"
        "    \"subnet\": \"2001:db8:1::/48\", "
        "    \"option-data\": [ {"
        "          \"name\": \"dns-servers\","
        "          \"space\": \"dhcp6\","
        "          \"code\": 23,"
        "          \"data\": \"2001:db8:1234:FFFF::1, 2001:db8:1234:FFFF::2\","
        "          \"csv-format\": True"
        "        },"
        "        {"
        "          \"name\": \"subscriber-id\","
        "          \"space\": \"dhcp6\","
        "          \"code\": 38,"
        "          \"data\": \"1234\","
        "          \"csv-format\": False"
        "        } ]"
        " } ],"
        "\"valid-lifetime\": 4000 }";

    ElementPtr json = Element::fromJSON(config);

    NakedDhcpv6Srv srv(0);

    EXPECT_NO_THROW(x = configureDhcp6Server(srv, json));
    ASSERT_TRUE(x);
    comment_ = parseAnswer(rcode_, x);

    ASSERT_EQ(0, rcode_);

    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    sol->addOption(generateIA(234, 1500, 3000));
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr adv = srv.processSolicit(sol);

    // check if we get response at all
    ASSERT_TRUE(adv);

    // We have not requested any options so they should not
    // be included in the response.
    ASSERT_FALSE(adv->getOption(D6O_SUBSCRIBER_ID));
    ASSERT_FALSE(adv->getOption(D6O_NAME_SERVERS));

    // Let's now request some options. We expect that the server
    // will include them in its response.
    boost::shared_ptr<OptionIntArray<uint16_t> >
        option_oro(new OptionIntArray<uint16_t>(Option::V6, D6O_ORO));
    // Create vector with two option codes.
    std::vector<uint16_t> codes(2);
    codes[0] = D6O_SUBSCRIBER_ID;
    codes[1] = D6O_NAME_SERVERS;
    // Pass this code to option.
    option_oro->setValues(codes);
    // Append ORO to SOLICIT message.
    sol->addOption(option_oro);

    // Need to process SOLICIT again after requesting new option.
    adv = srv.processSolicit(sol);
    ASSERT_TRUE(adv);

    OptionPtr tmp = adv->getOption(D6O_NAME_SERVERS);
    ASSERT_TRUE(tmp);

    boost::shared_ptr<Option6AddrLst> reply_nameservers =
        boost::dynamic_pointer_cast<Option6AddrLst>(tmp);
    ASSERT_TRUE(reply_nameservers);

    Option6AddrLst::AddressContainer addrs = reply_nameservers->getAddresses();
    ASSERT_EQ(2, addrs.size());
    EXPECT_TRUE(addrs[0] == IOAddress("2001:db8:1234:FFFF::1"));
    EXPECT_TRUE(addrs[1] == IOAddress("2001:db8:1234:FFFF::2"));

    // There is a dummy option with code 1000 we requested from a server.
    // Expect that this option is in server's response.
    tmp = adv->getOption(D6O_SUBSCRIBER_ID);
    ASSERT_TRUE(tmp);

    // Check that the option contains valid data (from configuration).
    std::vector<uint8_t> data = tmp->getData();
    ASSERT_EQ(2, data.size());

    const uint8_t foo_expected[] = {
        0x12, 0x34
    };
    EXPECT_EQ(0, memcmp(&data[0], foo_expected, 2));

    // more checks to be implemented
}


// There are no dedicated tests for Dhcpv6Srv::handleIA_NA and Dhcpv6Srv::assignLeases
// as they are indirectly tested in Solicit and Request tests.

// This test verifies that incoming SOLICIT can be handled properly, that an
// ADVERTISE is generated, that the response has an address and that address
// really belongs to the configured pool.
//
// This test sends a SOLICIT without any hint in IA_NA.
//
// constructed very simple SOLICIT message with:
// - client-id option (mandatory)
// - IA option (a request for address, without any addresses)
//
// expected returned ADVERTISE message:
// - copy of client-id
// - server-id
// - IA that includes IAADDR
TEST_F(Dhcpv6SrvTest, SolicitBasic) {
    NakedDhcpv6Srv srv(0);

    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    sol->addOption(generateIA(234, 1500, 3000));
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr reply = srv.processSolicit(sol);

    // check if we get response at all
    checkResponse(reply, DHCPV6_ADVERTISE, 1234);

    // check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr = checkIA_NA(reply, 234, subnet_->getT1(),
                                                subnet_->getT2());
    ASSERT_TRUE(addr);

    // Check that the assigned address is indeed from the configured pool
    checkIAAddr(addr, addr->getAddress(), subnet_->getPreferred(), subnet_->getValid());

    // check DUIDs
    checkServerId(reply, srv.getServerID());
    checkClientId(reply, clientid);
}

// This test verifies that incoming SOLICIT can be handled properly, that an
// ADVERTISE is generated, that the response has an address and that address
// really belongs to the configured pool.
//
// This test sends a SOLICIT with IA_NA that contains a valid hint.
//
// constructed very simple SOLICIT message with:
// - client-id option (mandatory)
// - IA option (a request for address, with an address that belongs to the
//              configured pool, i.e. is valid as hint)
//
// expected returned ADVERTISE message:
// - copy of client-id
// - server-id
// - IA that includes IAADDR
TEST_F(Dhcpv6SrvTest, SolicitHint) {
    NakedDhcpv6Srv srv(0);

    // Let's create a SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(234, 1500, 3000);

    // with a valid hint
    IOAddress hint("2001:db8:1:1::dead:beef");
    ASSERT_TRUE(subnet_->inPool(hint));
    OptionPtr hint_opt(new Option6IAAddr(D6O_IAADDR, hint, 300, 500));
    ia->addOption(hint_opt);
    sol->addOption(ia);
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr reply = srv.processSolicit(sol);

    // check if we get response at all
    checkResponse(reply, DHCPV6_ADVERTISE, 1234);

    OptionPtr tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);

    // check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr = checkIA_NA(reply, 234, subnet_->getT1(),
                                                subnet_->getT2());
    ASSERT_TRUE(addr);

    // check that we've got the address we requested
    checkIAAddr(addr, hint, subnet_->getPreferred(), subnet_->getValid());

    // check DUIDs
    checkServerId(reply, srv.getServerID());
    checkClientId(reply, clientid);
}

// This test verifies that incoming SOLICIT can be handled properly, that an
// ADVERTISE is generated, that the response has an address and that address
// really belongs to the configured pool.
//
// This test sends a SOLICIT with IA_NA that contains an invalid hint.
//
// constructed very simple SOLICIT message with:
// - client-id option (mandatory)
// - IA option (a request for address, with an address that does not
//              belong to the configured pool, i.e. is valid as hint)
//
// expected returned ADVERTISE message:
// - copy of client-id
// - server-id
// - IA that includes IAADDR
TEST_F(Dhcpv6SrvTest, SolicitInvalidHint) {
    NakedDhcpv6Srv srv(0);

    // Let's create a SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(234, 1500, 3000);
    IOAddress hint("2001:db8:1::cafe:babe");
    ASSERT_FALSE(subnet_->inPool(hint));
    OptionPtr hint_opt(new Option6IAAddr(D6O_IAADDR, hint, 300, 500));
    ia->addOption(hint_opt);
    sol->addOption(ia);
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr reply = srv.processSolicit(sol);

    // check if we get response at all
    checkResponse(reply, DHCPV6_ADVERTISE, 1234);

    // check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr = checkIA_NA(reply, 234, subnet_->getT1(),
                                                subnet_->getT2());
    ASSERT_TRUE(addr);

    // Check that the assigned address is indeed from the configured pool
    checkIAAddr(addr, addr->getAddress(), subnet_->getPreferred(), subnet_->getValid());
    EXPECT_TRUE(subnet_->inPool(addr->getAddress()));

    // check DUIDs
    checkServerId(reply, srv.getServerID());
    checkClientId(reply, clientid);
}

/// @todo: Add a test that client sends hint that is in pool, but currently
/// being used by a different client.

// This test checks that the server is offering different addresses to different
// clients in ADVERTISEs. Please note that ADVERTISE is not a guarantee that such
// an address will be assigned. Had the pool was very small and contained only
// 2 addresses, the third client would get the same advertise as the first one
// and this is a correct behavior. It is REQUEST that will fail for the third
// client. ADVERTISE is basically saying "if you send me a request, you will
// probably get an address like this" (there are no guarantees).
TEST_F(Dhcpv6SrvTest, ManySolicits) {
    NakedDhcpv6Srv srv(0);

    Pkt6Ptr sol1 = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    Pkt6Ptr sol2 = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 2345));
    Pkt6Ptr sol3 = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 3456));

    sol1->setRemoteAddr(IOAddress("fe80::abcd"));
    sol2->setRemoteAddr(IOAddress("fe80::1223"));
    sol3->setRemoteAddr(IOAddress("fe80::3467"));

    sol1->addOption(generateIA(1, 1500, 3000));
    sol2->addOption(generateIA(2, 1500, 3000));
    sol3->addOption(generateIA(3, 1500, 3000));

    // different client-id sizes
    OptionPtr clientid1 = generateClientId(12);
    OptionPtr clientid2 = generateClientId(14);
    OptionPtr clientid3 = generateClientId(16);

    sol1->addOption(clientid1);
    sol2->addOption(clientid2);
    sol3->addOption(clientid3);

    // Pass it to the server and get an advertise
    Pkt6Ptr reply1 = srv.processSolicit(sol1);
    Pkt6Ptr reply2 = srv.processSolicit(sol2);
    Pkt6Ptr reply3 = srv.processSolicit(sol3);

    // check if we get response at all
    checkResponse(reply1, DHCPV6_ADVERTISE, 1234);
    checkResponse(reply2, DHCPV6_ADVERTISE, 2345);
    checkResponse(reply3, DHCPV6_ADVERTISE, 3456);

    // check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr1 = checkIA_NA(reply1, 1, subnet_->getT1(),
                                                subnet_->getT2());
    boost::shared_ptr<Option6IAAddr> addr2 = checkIA_NA(reply2, 2, subnet_->getT1(),
                                                subnet_->getT2());
    boost::shared_ptr<Option6IAAddr> addr3 = checkIA_NA(reply3, 3, subnet_->getT1(),
                                                subnet_->getT2());
    ASSERT_TRUE(addr1);
    ASSERT_TRUE(addr2);
    ASSERT_TRUE(addr3);

    // Check that the assigned address is indeed from the configured pool
    checkIAAddr(addr1, addr1->getAddress(), subnet_->getPreferred(), subnet_->getValid());
    checkIAAddr(addr2, addr2->getAddress(), subnet_->getPreferred(), subnet_->getValid());
    checkIAAddr(addr3, addr3->getAddress(), subnet_->getPreferred(), subnet_->getValid());

    // check DUIDs
    checkServerId(reply1, srv.getServerID());
    checkServerId(reply2, srv.getServerID());
    checkServerId(reply3, srv.getServerID());
    checkClientId(reply1, clientid1);
    checkClientId(reply2, clientid2);
    checkClientId(reply3, clientid3);

    // Finally check that the addresses offered are different
    EXPECT_NE(addr1->getAddress().toText(), addr2->getAddress().toText());
    EXPECT_NE(addr2->getAddress().toText(), addr3->getAddress().toText());
    EXPECT_NE(addr3->getAddress().toText(), addr1->getAddress().toText());
    cout << "Offered address to client1=" << addr1->getAddress().toText() << endl;
    cout << "Offered address to client2=" << addr2->getAddress().toText() << endl;
    cout << "Offered address to client3=" << addr3->getAddress().toText() << endl;
}

// This test verifies that incoming REQUEST can be handled properly, that a
// REPLY is generated, that the response has an address and that address
// really belongs to the configured pool.
//
// This test sends a REQUEST with IA_NA that contains a valid hint.
//
// constructed very simple REQUEST message with:
// - client-id option (mandatory)
// - IA option (a request for address, with an address that belongs to the
//              configured pool, i.e. is valid as hint)
//
// expected returned REPLY message:
// - copy of client-id
// - server-id
// - IA that includes IAADDR
TEST_F(Dhcpv6SrvTest, RequestBasic) {
    NakedDhcpv6Srv srv(0);

    // Let's create a REQUEST
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_REQUEST, 1234));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(234, 1500, 3000);

    // with a valid hint
    IOAddress hint("2001:db8:1:1::dead:beef");
    ASSERT_TRUE(subnet_->inPool(hint));
    OptionPtr hint_opt(new Option6IAAddr(D6O_IAADDR, hint, 300, 500));
    ia->addOption(hint_opt);
    req->addOption(ia);
    OptionPtr clientid = generateClientId();
    req->addOption(clientid);

    // server-id is mandatory in REQUEST
    req->addOption(srv.getServerID());

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRequest(req);

    // check if we get response at all
    checkResponse(reply, DHCPV6_REPLY, 1234);

    OptionPtr tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);

    // check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr = checkIA_NA(reply, 234, subnet_->getT1(),
                                                subnet_->getT2());
    ASSERT_TRUE(addr);

    // check that we've got the address we requested
    checkIAAddr(addr, hint, subnet_->getPreferred(), subnet_->getValid());

    // check DUIDs
    checkServerId(reply, srv.getServerID());
    checkClientId(reply, clientid);

    // check that the lease is really in the database
    Lease6Ptr l = checkLease(duid_, reply->getOption(D6O_IA_NA), addr);
    EXPECT_TRUE(l);
    LeaseMgrFactory::instance().deleteLease(addr->getAddress());
}

// This test checks that the server is offering different addresses to different
// clients in REQUEST. Please note that ADVERTISE is not a guarantee that such
// and address will be assigned. Had the pool was very small and contained only
// 2 addresses, the third client would get the same advertise as the first one
// and this is a correct behavior. It is REQUEST that will fail for the third
// client. ADVERTISE is basically saying "if you send me a request, you will
// probably get an address like this" (there are no guarantees).
TEST_F(Dhcpv6SrvTest, ManyRequests) {
    NakedDhcpv6Srv srv(0);

    ASSERT_TRUE(subnet_);

    Pkt6Ptr req1 = Pkt6Ptr(new Pkt6(DHCPV6_REQUEST, 1234));
    Pkt6Ptr req2 = Pkt6Ptr(new Pkt6(DHCPV6_REQUEST, 2345));
    Pkt6Ptr req3 = Pkt6Ptr(new Pkt6(DHCPV6_REQUEST, 3456));

    req1->setRemoteAddr(IOAddress("fe80::abcd"));
    req2->setRemoteAddr(IOAddress("fe80::1223"));
    req3->setRemoteAddr(IOAddress("fe80::3467"));

    req1->addOption(generateIA(1, 1500, 3000));
    req2->addOption(generateIA(2, 1500, 3000));
    req3->addOption(generateIA(3, 1500, 3000));

    // different client-id sizes
    OptionPtr clientid1 = generateClientId(12);
    OptionPtr clientid2 = generateClientId(14);
    OptionPtr clientid3 = generateClientId(16);

    req1->addOption(clientid1);
    req2->addOption(clientid2);
    req3->addOption(clientid3);

    // server-id is mandatory in REQUEST
    req1->addOption(srv.getServerID());
    req2->addOption(srv.getServerID());
    req3->addOption(srv.getServerID());

    // Pass it to the server and get an advertise
    Pkt6Ptr reply1 = srv.processRequest(req1);
    Pkt6Ptr reply2 = srv.processRequest(req2);
    Pkt6Ptr reply3 = srv.processRequest(req3);

    // check if we get response at all
    checkResponse(reply1, DHCPV6_REPLY, 1234);
    checkResponse(reply2, DHCPV6_REPLY, 2345);
    checkResponse(reply3, DHCPV6_REPLY, 3456);

    // check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr1 = checkIA_NA(reply1, 1, subnet_->getT1(),
                                                subnet_->getT2());
    boost::shared_ptr<Option6IAAddr> addr2 = checkIA_NA(reply2, 2, subnet_->getT1(),
                                                subnet_->getT2());
    boost::shared_ptr<Option6IAAddr> addr3 = checkIA_NA(reply3, 3, subnet_->getT1(),
                                                subnet_->getT2());

    ASSERT_TRUE(addr1);
    ASSERT_TRUE(addr2);
    ASSERT_TRUE(addr3);

    // Check that the assigned address is indeed from the configured pool
    checkIAAddr(addr1, addr1->getAddress(), subnet_->getPreferred(), subnet_->getValid());
    checkIAAddr(addr2, addr2->getAddress(), subnet_->getPreferred(), subnet_->getValid());
    checkIAAddr(addr3, addr3->getAddress(), subnet_->getPreferred(), subnet_->getValid());

    // check DUIDs
    checkServerId(reply1, srv.getServerID());
    checkServerId(reply2, srv.getServerID());
    checkServerId(reply3, srv.getServerID());
    checkClientId(reply1, clientid1);
    checkClientId(reply2, clientid2);
    checkClientId(reply3, clientid3);

    // Finally check that the addresses offered are different
    EXPECT_NE(addr1->getAddress().toText(), addr2->getAddress().toText());
    EXPECT_NE(addr2->getAddress().toText(), addr3->getAddress().toText());
    EXPECT_NE(addr3->getAddress().toText(), addr1->getAddress().toText());
    cout << "Assigned address to client1=" << addr1->getAddress().toText() << endl;
    cout << "Assigned address to client2=" << addr2->getAddress().toText() << endl;
    cout << "Assigned address to client3=" << addr3->getAddress().toText() << endl;
}

// This test verifies that incoming (positive) RENEW can be handled properly, that a
// REPLY is generated, that the response has an address and that address
// really belongs to the configured pool and that lease is actually renewed.
//
// expected:
// - returned REPLY message has copy of client-id
// - returned REPLY message has server-id
// - returned REPLY message has IA that includes IAADDR
// - lease is actually renewed in LeaseMgr
TEST_F(Dhcpv6SrvTest, RenewBasic) {
    NakedDhcpv6Srv srv(0);

    const IOAddress addr("2001:db8:1:1::cafe:babe");
    const uint32_t iaid = 234;

    // Generate client-id also duid_
    OptionPtr clientid = generateClientId();

    // Check that the address we are about to use is indeed in pool
    ASSERT_TRUE(subnet_->inPool(addr));

    // Note that preferred, valid, T1 and T2 timers and CLTT are set to invalid
    // value on purpose. They should be updated during RENEW.
    Lease6Ptr lease(new Lease6(Lease6::LEASE_IA_NA, addr, duid_, iaid,
                               501, 502, 503, 504, subnet_->getID(), 0));
    lease->cltt_ = 1234;
    ASSERT_TRUE(LeaseMgrFactory::instance().addLease(lease));

    // Check that the lease is really in the database
    Lease6Ptr l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_TRUE(l);

    // Check that T1, T2, preferred, valid and cltt really set and not using
    // previous (500, 501, etc.) values
    EXPECT_NE(l->t1_, subnet_->getT1());
    EXPECT_NE(l->t2_, subnet_->getT2());
    EXPECT_NE(l->preferred_lft_, subnet_->getPreferred());
    EXPECT_NE(l->valid_lft_, subnet_->getValid());
    EXPECT_NE(l->cltt_, time(NULL));

    // Let's create a RENEW
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_RENEW, 1234));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(iaid, 1500, 3000);

    OptionPtr renewed_addr_opt(new Option6IAAddr(D6O_IAADDR, addr, 300, 500));
    ia->addOption(renewed_addr_opt);
    req->addOption(ia);
    req->addOption(clientid);

    // Server-id is mandatory in RENEW
    req->addOption(srv.getServerID());

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRenew(req);

    // Check if we get response at all
    checkResponse(reply, DHCPV6_REPLY, 1234);

    OptionPtr tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);

    // Check that IA_NA was returned and that there's an address included
    boost::shared_ptr<Option6IAAddr> addr_opt = checkIA_NA(reply, 234, subnet_->getT1(),
                                                           subnet_->getT2());

    ASSERT_TRUE(addr_opt);

    // Check that we've got the address we requested
    checkIAAddr(addr_opt, addr, subnet_->getPreferred(), subnet_->getValid());

    // Check DUIDs
    checkServerId(reply, srv.getServerID());
    checkClientId(reply, clientid);

    // Check that the lease is really in the database
    l = checkLease(duid_, reply->getOption(D6O_IA_NA), addr_opt);
    ASSERT_TRUE(l);

    // Check that T1, T2, preferred, valid and cltt were really updated
    EXPECT_EQ(l->t1_, subnet_->getT1());
    EXPECT_EQ(l->t2_, subnet_->getT2());
    EXPECT_EQ(l->preferred_lft_, subnet_->getPreferred());
    EXPECT_EQ(l->valid_lft_, subnet_->getValid());

    // Checking for CLTT is a bit tricky if we want to avoid off by 1 errors
    int32_t cltt = static_cast<int32_t>(l->cltt_);
    int32_t expected = static_cast<int32_t>(time(NULL));
    // equality or difference by 1 between cltt and expected is ok.
    EXPECT_GE(1, abs(cltt - expected));

    EXPECT_TRUE(LeaseMgrFactory::instance().deleteLease(addr_opt->getAddress()));
}

// This test verifies that incoming (invalid) RENEW can be handled properly.
//
// This test checks 3 scenarios:
// 1. there is no such lease at all
// 2. there is such a lease, but it is assigned to a different IAID
// 3. there is such a lease, but it belongs to a different client
//
// expected:
// - returned REPLY message has copy of client-id
// - returned REPLY message has server-id
// - returned REPLY message has IA that includes STATUS-CODE
// - No lease in LeaseMgr
TEST_F(Dhcpv6SrvTest, RenewReject) {
    NakedDhcpv6Srv srv(0);

    const IOAddress addr("2001:db8:1:1::dead");
    const uint32_t transid = 1234;
    const uint32_t valid_iaid = 234;
    const uint32_t bogus_iaid = 456;

    // Quick sanity check that the address we're about to use is ok
    ASSERT_TRUE(subnet_->inPool(addr));

    // GenerateClientId() also sets duid_
    OptionPtr clientid = generateClientId();

    // Check that the lease is NOT in the database
    Lease6Ptr l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_FALSE(l);

    // Let's create a RENEW
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_RENEW, transid));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(bogus_iaid, 1500, 3000);

    OptionPtr renewed_addr_opt(new Option6IAAddr(D6O_IAADDR, addr, 300, 500));
    ia->addOption(renewed_addr_opt);
    req->addOption(ia);
    req->addOption(clientid);

    // Server-id is mandatory in RENEW
    req->addOption(srv.getServerID());

    // Case 1: No lease known to server

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRenew(req);

    // Check if we get response at all
    checkResponse(reply, DHCPV6_REPLY, transid);
    OptionPtr tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    checkIA_NAStatusCode(ia, STATUS_NoBinding);

    // Check that there is no lease added
    l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_FALSE(l);

    // CASE 2: Lease is known and belongs to this client, but to a different IAID

    // Note that preferred, valid, T1 and T2 timers and CLTT are set to invalid
    // value on purpose. They should be updated during RENEW.
    Lease6Ptr lease(new Lease6(Lease6::LEASE_IA_NA, addr, duid_, valid_iaid,
                               501, 502, 503, 504, subnet_->getID(), 0));
    lease->cltt_ = 123; // Let's use it as an indicator that the lease
                        // was NOT updated.
    ASSERT_TRUE(LeaseMgrFactory::instance().addLease(lease));

    // Pass it to the server and hope for a REPLY
    reply = srv.processRenew(req);
    checkResponse(reply, DHCPV6_REPLY, transid);
    tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    checkIA_NAStatusCode(ia, STATUS_NoBinding);

    // There is a iaid mis-match, so server should respond that there is
    // no such address to renew.

    // CASE 3: Lease belongs to a client with different client-id
    req->delOption(D6O_CLIENTID);
    ia = boost::dynamic_pointer_cast<Option6IA>(req->getOption(D6O_IA_NA));
    ia->setIAID(valid_iaid); // Now iaid in renew matches that in leasemgr
    req->addOption(generateClientId(13)); // generate different DUID
                                          // (with length 13)

    reply = srv.processRenew(req);
    checkResponse(reply, DHCPV6_REPLY, transid);
    tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    checkIA_NAStatusCode(ia, STATUS_NoBinding);

    lease = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_TRUE(lease);
    // Verify that the lease was not updated.
    EXPECT_EQ(123, lease->cltt_);

    EXPECT_TRUE(LeaseMgrFactory::instance().deleteLease(addr));
}

// This test verifies that incoming (positive) RELEASE can be handled properly,
// that a REPLY is generated, that the response has status code and that the
// lease is indeed removed from the database.
//
// expected:
// - returned REPLY message has copy of client-id
// - returned REPLY message has server-id
// - returned REPLY message has IA that does not include an IAADDR
// - lease is actually removed from LeaseMgr
TEST_F(Dhcpv6SrvTest, ReleaseBasic) {
    NakedDhcpv6Srv srv(0);

    const IOAddress addr("2001:db8:1:1::cafe:babe");
    const uint32_t iaid = 234;

    // Generate client-id also duid_
    OptionPtr clientid = generateClientId();

    // Check that the address we are about to use is indeed in pool
    ASSERT_TRUE(subnet_->inPool(addr));

    // Note that preferred, valid, T1 and T2 timers and CLTT are set to invalid
    // value on purpose. They should be updated during RENEW.
    Lease6Ptr lease(new Lease6(Lease6::LEASE_IA_NA, addr, duid_, iaid,
                               501, 502, 503, 504, subnet_->getID(), 0));
    lease->cltt_ = 1234;
    ASSERT_TRUE(LeaseMgrFactory::instance().addLease(lease));

    // Check that the lease is really in the database
    Lease6Ptr l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_TRUE(l);

    // Let's create a RELEASE
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_RELEASE, 1234));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(iaid, 1500, 3000);

    OptionPtr released_addr_opt(new Option6IAAddr(D6O_IAADDR, addr, 300, 500));
    ia->addOption(released_addr_opt);
    req->addOption(ia);
    req->addOption(clientid);

    // Server-id is mandatory in RELEASE
    req->addOption(srv.getServerID());

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRelease(req);

    // Check if we get response at all
    checkResponse(reply, DHCPV6_REPLY, 1234);

    OptionPtr tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);

    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    checkIA_NAStatusCode(ia, STATUS_Success);
    checkMsgStatusCode(reply, STATUS_Success);

    // There should be no address returned in RELEASE (see RFC3315, 18.2.6)
    EXPECT_FALSE(tmp->getOption(D6O_IAADDR));

    // Check DUIDs
    checkServerId(reply, srv.getServerID());
    checkClientId(reply, clientid);

    // Check that the lease is really gone in the database
    // get lease by address
    l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_FALSE(l);

    // get lease by subnetid/duid/iaid combination
    l = LeaseMgrFactory::instance().getLease6(*duid_, iaid, subnet_->getID());
    ASSERT_FALSE(l);
}

// This test verifies that incoming (invalid) RELEASE can be handled properly.
//
// This test checks 3 scenarios:
// 1. there is no such lease at all
// 2. there is such a lease, but it is assigned to a different IAID
// 3. there is such a lease, but it belongs to a different client
//
// expected:
// - returned REPLY message has copy of client-id
// - returned REPLY message has server-id
// - returned REPLY message has IA that includes STATUS-CODE
// - No lease in LeaseMgr
TEST_F(Dhcpv6SrvTest, ReleaseReject) {

    NakedDhcpv6Srv srv(0);

    const IOAddress addr("2001:db8:1:1::dead");
    const uint32_t transid = 1234;
    const uint32_t valid_iaid = 234;
    const uint32_t bogus_iaid = 456;

    // Quick sanity check that the address we're about to use is ok
    ASSERT_TRUE(subnet_->inPool(addr));

    // GenerateClientId() also sets duid_
    OptionPtr clientid = generateClientId();

    // Check that the lease is NOT in the database
    Lease6Ptr l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_FALSE(l);

    // Let's create a RELEASE
    Pkt6Ptr req = Pkt6Ptr(new Pkt6(DHCPV6_RELEASE, transid));
    req->setRemoteAddr(IOAddress("fe80::abcd"));
    boost::shared_ptr<Option6IA> ia = generateIA(bogus_iaid, 1500, 3000);

    OptionPtr released_addr_opt(new Option6IAAddr(D6O_IAADDR, addr, 300, 500));
    ia->addOption(released_addr_opt);
    req->addOption(ia);
    req->addOption(clientid);

    // Server-id is mandatory in RENEW
    req->addOption(srv.getServerID());

    // Case 1: No lease known to server
    SCOPED_TRACE("CASE 1: No lease known to server");

    // Pass it to the server and hope for a REPLY
    Pkt6Ptr reply = srv.processRelease(req);

    // Check if we get response at all
    checkResponse(reply, DHCPV6_REPLY, transid);
    OptionPtr tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    checkIA_NAStatusCode(ia, STATUS_NoBinding);
    checkMsgStatusCode(reply, STATUS_NoBinding);

    // Check that the lease is not there
    l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_FALSE(l);

    // CASE 2: Lease is known and belongs to this client, but to a different IAID
    SCOPED_TRACE("CASE 2: Lease is known and belongs to this client, but to a different IAID");

    Lease6Ptr lease(new Lease6(Lease6::LEASE_IA_NA, addr, duid_, valid_iaid,
                               501, 502, 503, 504, subnet_->getID(), 0));
    ASSERT_TRUE(LeaseMgrFactory::instance().addLease(lease));

    // Pass it to the server and hope for a REPLY
    reply = srv.processRelease(req);
    checkResponse(reply, DHCPV6_REPLY, transid);
    tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    checkIA_NAStatusCode(ia, STATUS_NoBinding);
    checkMsgStatusCode(reply, STATUS_NoBinding);

    // Check that the lease is still there
    l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_TRUE(l);

    // CASE 3: Lease belongs to a client with different client-id
    SCOPED_TRACE("CASE 3: Lease belongs to a client with different client-id");

    req->delOption(D6O_CLIENTID);
    ia = boost::dynamic_pointer_cast<Option6IA>(req->getOption(D6O_IA_NA));
    ia->setIAID(valid_iaid); // Now iaid in renew matches that in leasemgr
    req->addOption(generateClientId(13)); // generate different DUID
                                          // (with length 13)

    reply = srv.processRelease(req);
    checkResponse(reply, DHCPV6_REPLY, transid);
    tmp = reply->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    // Check that IA_NA was returned and that there's an address included
    ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    checkIA_NAStatusCode(ia, STATUS_NoBinding);
    checkMsgStatusCode(reply, STATUS_NoBinding);

    // Check that the lease is still there
    l = LeaseMgrFactory::instance().getLease6(addr);
    ASSERT_TRUE(l);

    // Finally, let's cleanup the database
    EXPECT_TRUE(LeaseMgrFactory::instance().deleteLease(addr));
}

// This test verifies if the status code option is generated properly.
TEST_F(Dhcpv6SrvTest, StatusCode) {
    NakedDhcpv6Srv srv(0);

    // a dummy content for client-id
    uint8_t expected[] = {
        0x0, 0xD, // option code = 13
        0x0, 0x7, // option length = 7
        0x0, 0x3, // status code = 3
        0x41, 0x42, 0x43, 0x44, 0x45 // string value ABCDE
    };
    // Create the option.
    OptionPtr status = srv.createStatusCode(3, "ABCDE");
    // Allocate an output buffer. We will store the option
    // in wire format here.
    OutputBuffer buf(sizeof(expected));
    // Prepare the wire format.
    ASSERT_NO_THROW(status->pack(buf));
    // Check that the option buffer has valid length (option header + data).
    ASSERT_EQ(sizeof(expected), buf.getLength());
    // Verify the contents of the option.
    EXPECT_EQ(0, memcmp(expected, buf.getData(), sizeof(expected)));
}

// This test verifies if the sanityCheck() really checks options presence.
TEST_F(Dhcpv6SrvTest, sanityCheck) {
    NakedDhcpv6Srv srv(0);

    Pkt6Ptr pkt = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));

    // Set link-local sender address, so appropriate subnet can be
    // selected for this packet.
    pkt->setRemoteAddr(IOAddress("fe80::abcd"));

    // client-id is optional for information-request, so
    EXPECT_NO_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::OPTIONAL, Dhcpv6Srv::OPTIONAL));

    // empty packet, no client-id, no server-id
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::FORBIDDEN),
                 RFCViolation);

    // This doesn't make much sense, but let's check it for completeness
    EXPECT_NO_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::FORBIDDEN, Dhcpv6Srv::FORBIDDEN));

    OptionPtr clientid = generateClientId();
    pkt->addOption(clientid);

    // client-id is mandatory, server-id is forbidden (as in SOLICIT or REBIND)
    EXPECT_NO_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::FORBIDDEN));

    pkt->addOption(srv.getServerID());

    // both client-id and server-id are mandatory (as in REQUEST, RENEW, RELEASE, DECLINE)
    EXPECT_NO_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::MANDATORY));

    // sane section ends here, let's do some negative tests as well

    pkt->addOption(clientid);
    pkt->addOption(clientid);

    // with more than one client-id it should throw, no matter what
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::OPTIONAL, Dhcpv6Srv::OPTIONAL),
                 RFCViolation);
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::OPTIONAL),
                 RFCViolation);
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::OPTIONAL, Dhcpv6Srv::MANDATORY),
                 RFCViolation);
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::MANDATORY),
                 RFCViolation);

    pkt->delOption(D6O_CLIENTID);
    pkt->delOption(D6O_CLIENTID);

    // again we have only one client-id

    // let's try different type of insanity - several server-ids
    pkt->addOption(srv.getServerID());
    pkt->addOption(srv.getServerID());

    // with more than one server-id it should throw, no matter what
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::OPTIONAL, Dhcpv6Srv::OPTIONAL),
                 RFCViolation);
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::OPTIONAL),
                 RFCViolation);
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::OPTIONAL, Dhcpv6Srv::MANDATORY),
                 RFCViolation);
    EXPECT_THROW(srv.sanityCheck(pkt, Dhcpv6Srv::MANDATORY, Dhcpv6Srv::MANDATORY),
                 RFCViolation);
}

// This test verifies if selectSubnet() selects proper subnet for a given
// source address.
TEST_F(Dhcpv6SrvTest, selectSubnetAddr) {
    NakedDhcpv6Srv srv(0);

    Subnet6Ptr subnet1(new Subnet6(IOAddress("2001:db8:1::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet2(new Subnet6(IOAddress("2001:db8:2::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet3(new Subnet6(IOAddress("2001:db8:3::"), 48, 1, 2, 3, 4));

    // CASE 1: We have only one subnet defined and we received local traffic.
    // The only available subnet should be selected
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1); // just a single subnet

    Pkt6Ptr pkt = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    pkt->setRemoteAddr(IOAddress("fe80::abcd"));

    Subnet6Ptr selected = srv.selectSubnet(pkt);
    EXPECT_EQ(selected, subnet1);

    // CASE 2: We have only one subnet defined and we received relayed traffic.
    // We should NOT select it.

    // Identical steps as in case 1, but repeated for clarity
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1); // just a single subnet
    pkt->setRemoteAddr(IOAddress("2001:db8:abcd::2345"));
    selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

    // CASE 3: We have three subnets defined and we received local traffic.
    // Nothing should be selected.
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);
    pkt->setRemoteAddr(IOAddress("fe80::abcd"));
    selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

    // CASE 4: We have three subnets defined and we received relayed traffic
    // that came out of subnet 2. We should select subnet2 then
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);
    pkt->setRemoteAddr(IOAddress("2001:db8:2::baca"));
    selected = srv.selectSubnet(pkt);
    EXPECT_EQ(selected, subnet2);

    // CASE 5: We have three subnets defined and we received relayed traffic
    // that came out of undefined subnet. We should select nothing
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);
    pkt->setRemoteAddr(IOAddress("2001:db8:4::baca"));
    selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

}

// This test verifies if selectSubnet() selects proper subnet for a given
// network interface name.
TEST_F(Dhcpv6SrvTest, selectSubnetIface) {
    NakedDhcpv6Srv srv(0);

    Subnet6Ptr subnet1(new Subnet6(IOAddress("2001:db8:1::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet2(new Subnet6(IOAddress("2001:db8:2::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet3(new Subnet6(IOAddress("2001:db8:3::"), 48, 1, 2, 3, 4));

    subnet1->setIface("eth0");
    subnet3->setIface("wifi1");

    // CASE 1: We have only one subnet defined and it is available via eth0.
    // Packet came from eth0. The only available subnet should be selected
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1); // just a single subnet

    Pkt6Ptr pkt = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    pkt->setIface("eth0");

    Subnet6Ptr selected = srv.selectSubnet(pkt);
    EXPECT_EQ(selected, subnet1);

    // CASE 2: We have only one subnet defined and it is available via eth0.
    // Packet came from eth1. We should not select it
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1); // just a single subnet

    pkt->setIface("eth1");

    selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

    // CASE 3: We have only 3 subnets defined, one over eth0, one remote and
    // one over wifi1.
    // Packet came from eth1. We should not select it
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);

    pkt->setIface("eth0");
    EXPECT_EQ(subnet1, srv.selectSubnet(pkt));

    pkt->setIface("eth3"); // no such interface
    EXPECT_EQ(Subnet6Ptr(), srv.selectSubnet(pkt)); // nothing selected

    pkt->setIface("wifi1");
    EXPECT_EQ(subnet3, srv.selectSubnet(pkt));
}

// This test verifies if selectSubnet() selects proper subnet for a given
// linkaddr in RELAY-FORW message
TEST_F(Dhcpv6SrvTest, selectSubnetRelayLinkaddr) {
    NakedDhcpv6Srv srv(0);

    Subnet6Ptr subnet1(new Subnet6(IOAddress("2001:db8:1::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet2(new Subnet6(IOAddress("2001:db8:2::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet3(new Subnet6(IOAddress("2001:db8:3::"), 48, 1, 2, 3, 4));

    Pkt6::RelayInfo relay;
    relay.linkaddr_ = IOAddress("2001:db8:2::1234");
    relay.peeraddr_ = IOAddress("fe80::1");

    // CASE 1: We have only one subnet defined and we received relayed traffic.
    // The only available subnet should NOT be selected.
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1); // just a single subnet

    Pkt6Ptr pkt = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    pkt->relay_info_.push_back(relay);

    Subnet6Ptr selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

    // CASE 2: We have three subnets defined and we received relayed traffic.
    // Nothing should be selected.
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);
    selected = srv.selectSubnet(pkt);
    EXPECT_EQ(selected, subnet2);

    // CASE 3: We have three subnets defined and we received relayed traffic
    // that came out of subnet 2. We should select subnet2 then
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);

    // Source of the packet should have no meaning. Selection is based
    // on linkaddr field in the relay
    pkt->setRemoteAddr(IOAddress("2001:db8:1::baca"));
    selected = srv.selectSubnet(pkt);
    EXPECT_EQ(selected, subnet2);

    // CASE 4: We have three subnets defined and we received relayed traffic
    // that came out of undefined subnet. We should select nothing
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);
    pkt->relay_info_.clear();
    relay.linkaddr_ = IOAddress("2001:db8:4::1234");
    pkt->relay_info_.push_back(relay);
    selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

}

// This test verifies if selectSubnet() selects proper subnet for a given
// interface-id option
TEST_F(Dhcpv6SrvTest, selectSubnetRelayInterfaceId) {
    NakedDhcpv6Srv srv(0);

    Subnet6Ptr subnet1(new Subnet6(IOAddress("2001:db8:1::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet2(new Subnet6(IOAddress("2001:db8:2::"), 48, 1, 2, 3, 4));
    Subnet6Ptr subnet3(new Subnet6(IOAddress("2001:db8:3::"), 48, 1, 2, 3, 4));

    subnet1->setInterfaceId(generateInterfaceId("relay1"));
    subnet2->setInterfaceId(generateInterfaceId("relay2"));

    // CASE 1: We have only one subnet defined and it is for interface-id "relay1"
    // Packet came with interface-id "relay2". We should not select subnet1
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1); // just a single subnet

    Pkt6Ptr pkt = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    Pkt6::RelayInfo relay;
    relay.linkaddr_ = IOAddress("2001:db8:2::1234");
    relay.peeraddr_ = IOAddress("fe80::1");
    OptionPtr opt = generateInterfaceId("relay2");
    relay.options_.insert(make_pair(opt->getType(), opt));
    pkt->relay_info_.push_back(relay);

    // There is only one subnet configured and we are outside of that subnet
    Subnet6Ptr selected = srv.selectSubnet(pkt);
    EXPECT_FALSE(selected);

    // CASE 2: We have only one subnet defined and it is for interface-id "relay2"
    // Packet came with interface-id "relay2". We should select it
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet2); // just a single subnet
    selected = srv.selectSubnet(pkt);
    EXPECT_EQ(selected, subnet2);

    // CASE 3: We have only 3 subnets defined: one remote for interface-id "relay1",
    // one remote for interface-id "relay2" and third local
    // packet comes with interface-id "relay2". We should select subnet2
    CfgMgr::instance().deleteSubnets6();
    CfgMgr::instance().addSubnet6(subnet1);
    CfgMgr::instance().addSubnet6(subnet2);
    CfgMgr::instance().addSubnet6(subnet3);

    EXPECT_EQ(subnet2, srv.selectSubnet(pkt));
}

// This test verifies if the server-id disk operations (read, write) are
// working properly.
TEST_F(Dhcpv6SrvTest, ServerID) {
    NakedDhcpv6Srv srv(0);

    string duid1_text = "01:ff:02:03:06:80:90:ab:cd:ef";
    uint8_t duid1[] = { 0x01, 0xff, 2, 3, 6, 0x80, 0x90, 0xab, 0xcd, 0xef };
    OptionBuffer expected_duid1(duid1, duid1 + sizeof(duid1));

    fstream file1(DUID_FILE, ios::out | ios::trunc);
    file1 << duid1_text;
    file1.close();

    // Test reading from a file
    EXPECT_TRUE(srv.loadServerID(DUID_FILE));
    ASSERT_TRUE(srv.getServerID());
    ASSERT_EQ(sizeof(duid1) + Option::OPTION6_HDR_LEN, srv.getServerID()->len());
    ASSERT_TRUE(expected_duid1 == srv.getServerID()->getData());

    // Now test writing to a file
    EXPECT_EQ(0, unlink(DUID_FILE));
    EXPECT_NO_THROW(srv.writeServerID(DUID_FILE));

    fstream file2(DUID_FILE, ios::in);
    ASSERT_TRUE(file2.good());
    string text;
    file2 >> text;
    file2.close();

    EXPECT_EQ(duid1_text, text);
}

// Checks if hooks are implemented properly.
TEST_F(Dhcpv6SrvTest, Hooks) {
    NakedDhcpv6Srv srv(0);

    // check if appropriate hooks are registered

    // check if appropriate indexes are set
    int hook_index_pkt6_received = ServerHooks::getServerHooks().getIndex("pkt6_receive");
    int hook_index_select_subnet = ServerHooks::getServerHooks().getIndex("subnet6_select");
    int hook_index_pkt6_send     = ServerHooks::getServerHooks().getIndex("pkt6_send");

    EXPECT_TRUE(hook_index_pkt6_received > 0);
    EXPECT_TRUE(hook_index_select_subnet > 0);
    EXPECT_TRUE(hook_index_pkt6_send > 0);
}

// This function returns buffer for empty packet (just DHCPv6 header)
Pkt6* captureEmpty() {
    Pkt6* pkt;
    uint8_t data[4];
    data[0] = 1; // type 1 = SOLICIT
    data[1] = 0xca; // trans-id = 0xcafe01
    data[2] = 0xfe;
    data[3] = 0x01;

    pkt = new Pkt6(data, sizeof(data));
    pkt->setRemotePort(546);
    pkt->setRemoteAddr(IOAddress("fe80::1"));
    pkt->setLocalPort(0);
    pkt->setLocalAddr(IOAddress("ff02::1:2"));
    pkt->setIndex(2);
    pkt->setIface("eth0");

    return (pkt);
}

// This function returns buffer for very simple Solicit
Pkt6* captureSimpleSolicit() {
    Pkt6* pkt;
    uint8_t data[] = {
        1,  // type 1 = SOLICIT
        0xca, 0xfe, 0x01, // trans-id = 0xcafe01
        0, 1, // option type 1 (client-id)
        0, 10, // option lenth 10
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, // DUID
        0, 3, // option type 3 (IA_NA)
        0, 12, // option length 12
        0, 0, 0, 1, // iaid = 1
        0, 0, 0, 0, // T1 = 0
        0, 0, 0, 0  // T2 = 0
    };

    pkt = new Pkt6(data, sizeof(data));
    pkt->setRemotePort(546);
    pkt->setRemoteAddr(IOAddress("fe80::1"));
    pkt->setLocalPort(0);
    pkt->setLocalAddr(IOAddress("ff02::1:2"));
    pkt->setIndex(2);
    pkt->setIface("eth0");

    return (pkt);
}

/// @brief a class dedicated to Hooks testing in DHCPv6 server
///
/// This class has a some static members, because each non-static
/// method has implicit this parameter, so it does not match callout
/// signature and couldn't be registered. Furthermore, static methods
/// can't modify non-static members (for obvious reasons), so many
/// fields are declared static. It is still better to keep them as
/// one class rather than unrelated collection of global objects.
class HooksDhcpv6SrvTest : public Dhcpv6SrvTest {

public:
    HooksDhcpv6SrvTest() {

        // Allocate new DHCPv6 Server
        srv_ = new NakedDhcpv6Srv(0);

        // clear static buffers
        resetCalloutBuffers();
    }

    ~HooksDhcpv6SrvTest() {
        delete srv_;
    }

    static OptionPtr createOption(uint16_t option_code) {

        char payload[] = {
            0xa, 0xb, 0xc, 0xe, 0xf, 0x10, 0x11, 0x12, 0x13, 0x14
        };

        OptionBuffer tmp(payload, payload + sizeof(payload));
        return OptionPtr(new Option(Option::V6, option_code, tmp));
    }

    /// callback that stores received callout name and pkt6 value
    static int
    pkt6_receive_callout(CalloutHandle& callout_handle) {
        callback_name_ = string("pkt6_receive");

        callout_handle.getArgument("pkt6", callback_pkt6_);

        callback_argument_names_ = callout_handle.getArgumentNames();
        return (0);
    }

    // callback that changes client-id value
    static int
    pkt6_receive_change_clientid(CalloutHandle& callout_handle) {

        Pkt6Ptr pkt;
        callout_handle.getArgument("pkt6", pkt);

        // get rid of the old client-id
        pkt->delOption(D6O_CLIENTID);

        // add a new option
        pkt->addOption(createOption(D6O_CLIENTID));

        // carry on as usual
        return pkt6_receive_callout(callout_handle);
    }

    // callback that deletes client-id
    static int
    pkt6_receive_delete_clientid(CalloutHandle& callout_handle) {

        Pkt6Ptr pkt;
        callout_handle.getArgument("pkt6", pkt);

        // get rid of the old client-id
        pkt->delOption(D6O_CLIENTID);

        // carry on as usual
        return pkt6_receive_callout(callout_handle);
    }

    // callback that sets skip flag
    static int
    pkt6_receive_skip(CalloutHandle& callout_handle) {

        Pkt6Ptr pkt;
        callout_handle.getArgument("pkt6", pkt);

        callout_handle.setSkip(true);

        // carry on as usual
        return pkt6_receive_callout(callout_handle);
    }

    // Callback that stores received callout name and pkt6 value
    static int
    pkt6_send_callout(CalloutHandle& callout_handle) {
        callback_name_ = string("pkt6_send");

        callout_handle.getArgument("pkt6", callback_pkt6_);

        callback_argument_names_ = callout_handle.getArgumentNames();
        return (0);
    }

    // Callback that changes server-id
    static int
    pkt6_send_change_serverid(CalloutHandle& callout_handle) {

        Pkt6Ptr pkt;
        callout_handle.getArgument("pkt6", pkt);

        // get rid of the old server-id
        pkt->delOption(D6O_SERVERID);

        // add a new option
        pkt->addOption(createOption(D6O_SERVERID));

        // carry on as usual
        return pkt6_send_callout(callout_handle);
    }

    // callback that deletes server-id
    static int
    pkt6_send_delete_serverid(CalloutHandle& callout_handle) {

        Pkt6Ptr pkt;
        callout_handle.getArgument("pkt6", pkt);

        // get rid of the old client-id
        pkt->delOption(D6O_SERVERID);

        // carry on as usual
        return pkt6_send_callout(callout_handle);
    }

    // Callback that sets skip blag
    static int
    pkt6_send_skip(CalloutHandle& callout_handle) {

        Pkt6Ptr pkt;
        callout_handle.getArgument("pkt6", pkt);

        callout_handle.setSkip(true);

        // carry on as usual
        return pkt6_send_callout(callout_handle);
    }

    // Callback that stores received callout name and subnet6 values
    static int
    subnet6_select_callout(CalloutHandle& callout_handle) {
        callback_name_ = string("subnet6_select");

        callout_handle.getArgument("pkt6", callback_pkt6_);
        callout_handle.getArgument("subnet6", callback_subnet6_);
        callout_handle.getArgument("subnet6collection", callback_subnet6collection_);

        callback_argument_names_ = callout_handle.getArgumentNames();
        return (0);
    }

    // Callback that picks the other subnet if possible.
    static int
    subnet6_select_different_subnet_callout(CalloutHandle& callout_handle) {

        // Call the basic calllout to record all passed values
        subnet6_select_callout(callout_handle);

        Subnet6Collection subnets;
        Subnet6Ptr subnet;
        callout_handle.getArgument("subnet6", subnet);
        callout_handle.getArgument("subnet6collection", subnets);

        // Let's change to a different subnet
        if (subnets.size() > 1) {
            subnet = subnets[1]; // Let's pick the other subnet
            callout_handle.setArgument("subnet6", subnet);
        }

        return (0);
    }

    void resetCalloutBuffers() {
        callback_name_ = string("");
        callback_pkt6_.reset();
        callback_subnet6_.reset();
        callback_subnet6collection_.clear();
        callback_argument_names_.clear();
    }

    NakedDhcpv6Srv* srv_;

    // Used in testing pkt6_receive_callout
    static string callback_name_; ///< string name of the received callout

    static Pkt6Ptr callback_pkt6_; ///< Pkt6 structure returned in the callout

    static Subnet6Ptr callback_subnet6_;

    static Subnet6Collection callback_subnet6collection_;

    static vector<string> callback_argument_names_;
};

string HooksDhcpv6SrvTest::callback_name_;

Pkt6Ptr HooksDhcpv6SrvTest::callback_pkt6_;

Subnet6Ptr HooksDhcpv6SrvTest::callback_subnet6_;

Subnet6Collection HooksDhcpv6SrvTest::callback_subnet6collection_;

vector<string> HooksDhcpv6SrvTest::callback_argument_names_;


// Checks if callouts installed on pkt6_received are indeed called and the
// all necessary parameters are passed.
//
// Note that the test name does not follow test naming convention,
// but the proper hook name is "pkt6_receive".
TEST_F(HooksDhcpv6SrvTest, simple_pkt6_receive) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_receive", pkt6_receive_callout));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // check that the callback called is indeed the one we installed
    EXPECT_EQ("pkt6_receive", callback_name_);

    // check that pkt6 argument passing was successful and returned proper value
    EXPECT_TRUE(callback_pkt6_.get() == sol.get());

    // Check that all expected parameters are there
    vector<string> expected_argument_names;
    expected_argument_names.push_back(string("pkt6"));

    EXPECT_TRUE(expected_argument_names == callback_argument_names_);
}

// Checks if callouts installed on pkt6_received is able to change
// the values and the parameters are indeed used by the server.
TEST_F(HooksDhcpv6SrvTest, valueChange_pkt6_receive) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_receive", pkt6_receive_change_clientid));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // check that the server did send a reposonce
    ASSERT_EQ(1, srv_->fake_sent_.size());

    // Make sure that we received a response
    Pkt6Ptr adv = srv_->fake_sent_.front();
    ASSERT_TRUE(adv);

    // Get client-id...
    OptionPtr clientid = adv->getOption(D6O_CLIENTID);

    // ... and check if it is the modified value
    OptionPtr expected = createOption(D6O_CLIENTID);
    EXPECT_TRUE(clientid->equal(expected));
}

// Checks if callouts installed on pkt6_received is able to delete
// existing options and that change impacts server processing (mandatory
// client-id option is deleted, so the packet is expected to be dropped)
TEST_F(HooksDhcpv6SrvTest, deleteClientId_pkt6_receive) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_receive", pkt6_receive_delete_clientid));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // Check that the server dropped the packet and did not send a response
    ASSERT_EQ(0, srv_->fake_sent_.size());
}

// Checks if callouts installed on pkt6_received is able to set skip flag that
// will cause the server to not process the packet (drop), even though it is valid.
TEST_F(HooksDhcpv6SrvTest, skip_pkt6_receive) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_receive", pkt6_receive_skip));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // check that the server dropped the packet and did not produce any response
    ASSERT_EQ(0, srv_->fake_sent_.size());
}


// Checks if callouts installed on pkt6_send are indeed called and the
// all necessary parameters are passed.
TEST_F(HooksDhcpv6SrvTest, simple_pkt6_send) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_send", pkt6_send_callout));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // Check that the callback called is indeed the one we installed
    EXPECT_EQ("pkt6_send", callback_name_);

    // Check that there is one packet sent
    ASSERT_EQ(1, srv_->fake_sent_.size());
    Pkt6Ptr adv = srv_->fake_sent_.front();

    // Check that pkt6 argument passing was successful and returned proper value
    EXPECT_TRUE(callback_pkt6_.get() == adv.get());

    // Check that all expected parameters are there
    vector<string> expected_argument_names;
    expected_argument_names.push_back(string("pkt6"));
    EXPECT_TRUE(expected_argument_names == callback_argument_names_);
}

// Checks if callouts installed on pkt6_send is able to change
// the values and the packet sent contains those changes
TEST_F(HooksDhcpv6SrvTest, valueChange_pkt6_send) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_send", pkt6_send_change_serverid));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // check that the server did send a reposonce
    ASSERT_EQ(1, srv_->fake_sent_.size());

    // Make sure that we received a response
    Pkt6Ptr adv = srv_->fake_sent_.front();
    ASSERT_TRUE(adv);

    // Get client-id...
    OptionPtr clientid = adv->getOption(D6O_SERVERID);

    // ... and check if it is the modified value
    OptionPtr expected = createOption(D6O_SERVERID);
    EXPECT_TRUE(clientid->equal(expected));
}

// Checks if callouts installed on pkt6_send is able to delete
// existing options and that server applies those changes. In particular,
// we are trying to send a packet without server-id. The packet should
// be sent
TEST_F(HooksDhcpv6SrvTest, deleteServerId_pkt6_send) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_send", pkt6_send_delete_serverid));

    // Let's create a simple SOLICIT
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // Check that the server indeed sent a malformed ADVERTISE
    ASSERT_EQ(1, srv_->fake_sent_.size());

    // Get that ADVERTISE
    Pkt6Ptr adv = srv_->fake_sent_.front();
    ASSERT_TRUE(adv);

    // Make sure that it does not have server-id
    EXPECT_FALSE(adv->getOption(D6O_SERVERID));
}

// Checks if callouts installed on pkt6_skip is able to set skip flag that
// will cause the server to not process the packet (drop), even though it is valid.
TEST_F(HooksDhcpv6SrvTest, skip_pkt6_send) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "pkt6_send", pkt6_send_skip));

    // Let's create a simple REQUEST
    Pkt6Ptr sol = Pkt6Ptr(captureSimpleSolicit());

    // Simulate that we have received that traffic
    srv_->fakeReceive(sol);

    // Server will now process to run its normal loop, but instead of calling
    // IfaceMgr::receive6(), it will read all packets from the list set by
    // fakeReceive()
    // In particular, it should call registered pkt6_receive callback.
    srv_->run();

    // check that the server dropped the packet and did not produce any response
    ASSERT_EQ(0, srv_->fake_sent_.size());
}

// This test checks if subnet6_select callout is triggered and reports
// valid parameters
TEST_F(HooksDhcpv6SrvTest, subnet6_select) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "subnet6_select", subnet6_select_callout));

    // Configure 2 subnets, both directly reachable over local interface
    // (let's not complicate the matter with relays)
    string config = "{ \"interface\": [ \"all\" ],"
        "\"preferred-lifetime\": 3000,"
        "\"rebind-timer\": 2000, "
        "\"renew-timer\": 1000, "
        "\"subnet6\": [ { "
        "    \"pool\": [ \"2001:db8:1::/64\" ],"
        "    \"subnet\": \"2001:db8:1::/48\", "
        "    \"interface\": \"" + valid_iface_ + "\" "
        " }, {"
        "    \"pool\": [ \"2001:db8:2::/64\" ],"
        "    \"subnet\": \"2001:db8:2::/48\" "
        " } ],"
        "\"valid-lifetime\": 4000 }";

    ElementPtr json = Element::fromJSON(config);
    ConstElementPtr status;

    // Configure the server and make sure the config is accepted
    EXPECT_NO_THROW(status = configureDhcp6Server(*srv_, json));
    ASSERT_TRUE(status);
    comment_ = parseAnswer(rcode_, status);
    ASSERT_EQ(0, rcode_);

    // Prepare solicit packet. Server should select first subnet for it
    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    sol->setIface(valid_iface_);
    sol->addOption(generateIA(234, 1500, 3000));
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr adv = srv_->processSolicit(sol);

    // check if we get response at all
    ASSERT_TRUE(adv);

    // Check that the callback called is indeed the one we installed
    EXPECT_EQ("subnet6_select", callback_name_);

    // Check that pkt6 argument passing was successful and returned proper value
    EXPECT_TRUE(callback_pkt6_.get() == sol.get());

    Subnet6Collection exp_subnets = CfgMgr::instance().getSubnets6();

    // The server is supposed to pick the first subnet, because of matching
    // interface. Check that the value is reported properly.
    ASSERT_TRUE(callback_subnet6_);
    EXPECT_EQ(callback_subnet6_.get(), exp_subnets.front().get());

    // Server is supposed to report two subnets
    ASSERT_EQ(exp_subnets.size(), callback_subnet6collection_.size());

    // Compare that the available subnets are reported as expected
    EXPECT_TRUE(exp_subnets[0].get() == callback_subnet6collection_[0].get());
    EXPECT_TRUE(exp_subnets[1].get() == callback_subnet6collection_[1].get());
}

// This test checks if callout installed on subnet6_select hook point can pick
// a different subnet.
TEST_F(HooksDhcpv6SrvTest, subnet_select_change) {

    // Install pkt6_receive_callout
    EXPECT_NO_THROW(HooksManager::preCalloutsLibraryHandle().registerCallout(
                        "subnet6_select", subnet6_select_different_subnet_callout));

    // Configure 2 subnets, both directly reachable over local interface
    // (let's not complicate the matter with relays)
    string config = "{ \"interface\": [ \"all\" ],"
        "\"preferred-lifetime\": 3000,"
        "\"rebind-timer\": 2000, "
        "\"renew-timer\": 1000, "
        "\"subnet6\": [ { "
        "    \"pool\": [ \"2001:db8:1::/64\" ],"
        "    \"subnet\": \"2001:db8:1::/48\", "
        "    \"interface\": \"" + valid_iface_ + "\" "
        " }, {"
        "    \"pool\": [ \"2001:db8:2::/64\" ],"
        "    \"subnet\": \"2001:db8:2::/48\" "
        " } ],"
        "\"valid-lifetime\": 4000 }";

    ElementPtr json = Element::fromJSON(config);
    ConstElementPtr status;

    // Configure the server and make sure the config is accepted
    EXPECT_NO_THROW(status = configureDhcp6Server(*srv_, json));
    ASSERT_TRUE(status);
    comment_ = parseAnswer(rcode_, status);
    ASSERT_EQ(0, rcode_);

    // Prepare solicit packet. Server should select first subnet for it
    Pkt6Ptr sol = Pkt6Ptr(new Pkt6(DHCPV6_SOLICIT, 1234));
    sol->setRemoteAddr(IOAddress("fe80::abcd"));
    sol->setIface(valid_iface_);
    sol->addOption(generateIA(234, 1500, 3000));
    OptionPtr clientid = generateClientId();
    sol->addOption(clientid);

    // Pass it to the server and get an advertise
    Pkt6Ptr adv = srv_->processSolicit(sol);

    // check if we get response at all
    ASSERT_TRUE(adv);

    // The response should have an address from second pool, so let's check it
    OptionPtr tmp = adv->getOption(D6O_IA_NA);
    ASSERT_TRUE(tmp);
    boost::shared_ptr<Option6IA> ia = boost::dynamic_pointer_cast<Option6IA>(tmp);
    ASSERT_TRUE(ia);
    tmp = ia->getOption(D6O_IAADDR);
    ASSERT_TRUE(tmp);
    boost::shared_ptr<Option6IAAddr> addr_opt =
        boost::dynamic_pointer_cast<Option6IAAddr>(tmp);
    ASSERT_TRUE(addr_opt);

    // Get all subnets and use second subnet for verification
    Subnet6Collection subnets = CfgMgr::instance().getSubnets6();
    ASSERT_EQ(2, subnets.size());

    // Advertised address must belong to the second pool (in subnet's range,
    // in dynamic pool)
    EXPECT_TRUE(subnets[1]->inRange(addr_opt->getAddress()));
    EXPECT_TRUE(subnets[1]->inPool(addr_opt->getAddress()));
}


/// @todo: Add more negative tests for processX(), e.g. extend sanityCheck() test
/// to call processX() methods.

}   // end of anonymous namespace
