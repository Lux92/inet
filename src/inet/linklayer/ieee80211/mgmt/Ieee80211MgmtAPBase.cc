//
// Copyright (C) 2006 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/linklayer/ieee80211/mgmt/Ieee80211MgmtAPBase.h"

#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/common/EtherTypeTag_m.h"
#include "inet/linklayer/common/Ieee802Ctrl.h"
#include "inet/linklayer/common/MACAddressTag_m.h"
#include "inet/linklayer/common/UserPriorityTag_m.h"
#include <string.h>

#ifdef WITH_ETHERNET
#include "inet/linklayer/ethernet/EtherEncap.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#endif // ifdef WITH_ETHERNET


namespace inet {

namespace ieee80211 {

void Ieee80211MgmtAPBase::initialize(int stage)
{
    Ieee80211MgmtBase::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        isConnectedToHL = gate("upperLayerOut")->getPathEndGate()->isConnected();
        const char *encDec = par("encapDecap").stringValue();
        if (!strcmp(encDec, "true"))
            encapDecap = ENCAP_DECAP_TRUE;
        else if (!strcmp(encDec, "false"))
            encapDecap = ENCAP_DECAP_FALSE;
        else if (!strcmp(encDec, "eth"))
            encapDecap = ENCAP_DECAP_ETH;
        else
            throw cRuntimeError("Unknown encapDecap parameter value: '%s'! Must be 'true','false' or 'eth'.", encDec);

        WATCH(isConnectedToHL);
    }
}

void Ieee80211MgmtAPBase::distributeReceivedDataFrame(Ieee80211DataFrame *frame)
{
    // adjust toDS/fromDS bits, and shuffle addresses
    frame->setToDS(false);
    frame->setFromDS(true);

    // move destination address to address1 (receiver address),
    // and fill address3 with original source address;
    // sender address (address2) will be filled in by MAC
    ASSERT(!frame->getAddress3().isUnspecified());
    frame->setReceiverAddress(frame->getAddress3());
    frame->setAddress3(frame->getTransmitterAddress());

    sendDown(frame);
}

void Ieee80211MgmtAPBase::sendToUpperLayer(Ieee80211DataFrame *frame)
{
    if (!isConnectedToHL) {
        delete frame;
        return;
    }
    cPacket *outFrame = nullptr;
    switch (encapDecap) {
        case ENCAP_DECAP_ETH:
#ifdef WITH_ETHERNET
            outFrame = convertToEtherFrame(frame);
#else // ifdef WITH_ETHERNET
            throw cRuntimeError("INET compiled without ETHERNET feature, but the 'encapDecap' parameter is set to 'eth'!");
#endif // ifdef WITH_ETHERNET
            break;

        case ENCAP_DECAP_TRUE: {
            cPacket *payload = frame->decapsulate();
            auto macAddressInd = payload->ensureTag<MacAddressInd>();
            macAddressInd->setSrcAddress(frame->getTransmitterAddress());
            macAddressInd->setDestAddress(frame->getAddress3());
            int tid = frame->getTid();
            if (tid < 8)
                payload->ensureTag<UserPriorityInd>()->setUserPriority(tid); // TID values 0..7 are UP
            Ieee80211DataFrameWithSNAP *frameWithSNAP = dynamic_cast<Ieee80211DataFrameWithSNAP *>(frame);
            if (frameWithSNAP) {
                int etherType = frameWithSNAP->getEtherType();
                payload->ensureTag<EtherTypeInd>()->setEtherType(etherType);
                payload->ensureTag<DispatchProtocolReq>()->setProtocol(ProtocolGroup::ethertype.getProtocol(etherType));
            }
            delete frame;
            outFrame = payload;
        }
        break;

        case ENCAP_DECAP_FALSE:
            outFrame = frame;
            break;

        default:
            throw cRuntimeError("Unknown encapDecap value: %d", encapDecap);
            break;
    }
    sendUp(outFrame);
}

Packet *Ieee80211MgmtAPBase::convertToEtherFrame(Ieee80211DataFrame *frame_)
{
#ifdef WITH_ETHERNET
    Ieee80211DataFrameWithSNAP *frame = check_and_cast<Ieee80211DataFrameWithSNAP *>(frame_);

    // create a matching ethernet frame
    const auto& ethframe = std::make_shared<EthernetIIFrame>();    //TODO option to use EtherFrameWithSNAP instead
    ethframe->setDest(frame->getAddress3());
    ethframe->setSrc(frame->getTransmitterAddress());
    ethframe->setEtherType(frame->getEtherType());
    ethframe->setChunkLength(byte(ETHER_MAC_FRAME_BYTES));

    // encapsulate the payload in there
    Packet *pk = check_and_cast<Packet*>(frame->decapsulate());
    delete frame;
    ethframe->markImmutable();
    pk->pushHeader(ethframe);
    EtherEncap::addPaddingAndFcs(pk);

    pk->ensureTag<DispatchProtocolReq>()->setProtocol(&Protocol::ethernet);
    pk->ensureTag<PacketProtocolTag>()->setProtocol(&Protocol::ethernet);

    // done
    return pk;
#else // ifdef WITH_ETHERNET
    throw cRuntimeError("INET compiled without ETHERNET feature!");
#endif // ifdef WITH_ETHERNET
}

Ieee80211DataFrame *Ieee80211MgmtAPBase::convertFromEtherFrame(Packet *packet)
{
#ifdef WITH_ETHERNET
    auto ethframe = CHK(EtherEncap::decapsulate(packet));       // do not use const auto& : removePoppedChunks() delete it from packet
    // create new frame
    Ieee80211DataFrameWithSNAP *frame = new Ieee80211DataFrameWithSNAP(ethframe->getName());
    frame->setFromDS(true);
    packet->removePoppedChunks();

    // copy addresses from ethernet frame (transmitter addr will be set to our addr by MAC)
    frame->setReceiverAddress(ethframe->getDest());
    frame->setAddress3(ethframe->getSrc());

    // copy EtherType from original frame
    if (const auto& eth2frame = std::dynamic_pointer_cast<EthernetIIFrame>(ethframe))
        frame->setEtherType(eth2frame->getEtherType());
    else if (const auto& snapframe = std::dynamic_pointer_cast<EtherFrameWithSNAP>(ethframe))
        frame->setEtherType(snapframe->getLocalcode());
    else
        throw cRuntimeError("Unaccepted EtherFrame type: %s, contains no EtherType", ethframe->getClassName());

    // encapsulate payload
    frame->encapsulate(packet);

    // done
    return frame;
#else // ifdef WITH_ETHERNET
    throw cRuntimeError("INET compiled without ETHERNET feature!");
#endif // ifdef WITH_ETHERNET
}

Ieee80211DataFrame *Ieee80211MgmtAPBase::encapsulate(cPacket *msg)
{
    switch (encapDecap) {
        case ENCAP_DECAP_ETH:
#ifdef WITH_ETHERNET
            return convertFromEtherFrame(check_and_cast<Packet *>(msg));
#else // ifdef WITH_ETHERNET
            throw cRuntimeError("INET compiled without ETHERNET feature, but the 'encapDecap' parameter is set to 'eth'!");
#endif // ifdef WITH_ETHERNET
            break;

        case ENCAP_DECAP_TRUE: {
            Ieee80211DataFrameWithSNAP *frame = new Ieee80211DataFrameWithSNAP(msg->getName());
            frame->setFromDS(true);

            // copy addresses from ethernet frame (transmitter addr will be set to our addr by MAC)
            frame->setAddress3(msg->getMandatoryTag<MacAddressReq>()->getSrcAddress());
            frame->setReceiverAddress(msg->getMandatoryTag<MacAddressReq>()->getDestAddress());
            frame->setEtherType(msg->getMandatoryTag<EtherTypeReq>()->getEtherType());
            auto userPriorityReq = msg->getTag<UserPriorityReq>();
            if (userPriorityReq != nullptr) {
                // make it a QoS frame, and set TID
                frame->setType(ST_DATA_WITH_QOS);
                frame->addBitLength(QOSCONTROL_BITS);
                frame->setTid(userPriorityReq->getUserPriority());
            }

            // encapsulate payload
            frame->encapsulate(msg);
            return frame;
        }
        break;

        case ENCAP_DECAP_FALSE:
            return check_and_cast<Ieee80211DataFrame *>(msg);
            break;

        default:
            throw cRuntimeError("Unknown encapDecap value: %d", encapDecap);
            break;
    }
    return nullptr;
}

} // namespace ieee80211

} // namespace inet

