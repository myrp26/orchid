/* Orchid - WebRTC P2P VPN Market (on Ethereum)
 * Copyright (C) 2017-2019  The Orchid Authors
*/

/* GNU Affero General Public License, Version 3 {{{ */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.

 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/
/* }}} */


#include <api/jsep_session_description.h>
#include <pc/webrtc_sdp.h>

#include "cashier.hpp"
#include "channel.hpp"
#include "crypto.hpp"
#include "datagram.hpp"
#include "endpoint.hpp"
#include "local.hpp"
#include "protocol.hpp"
#include "server.hpp"
#include "spawn.hpp"

namespace orc {

class Incoming final :
    public Peer
{
  private:
    S<Incoming> self_;
  private:
    S<Server> server_;

  protected:
    void Land(rtc::scoped_refptr<webrtc::DataChannelInterface> interface) override {
        auto &bonding(server_->Bond());
        auto &channel(bonding.Wire<Channel>(shared_from_this(), interface));

        Spawn([&bonding, &channel, server = std::move(server_)]() noexcept -> task<void> {
            co_await channel.Open();
            // XXX: this could fail; then what?
            co_await server->Open(bonding);
        });
    }

    void Stop(const std::string &error) noexcept override {
        self_.reset();
    }

  public:
    Incoming(S<Server> server, const S<Origin> &origin, rtc::scoped_refptr<rtc::RTCCertificate> local, std::vector<std::string> ice) :
        Peer(origin, [&]() {
            Configuration configuration;
            configuration.tls_ = std::move(local);
            configuration.ice_ = std::move(ice);
            return configuration;
        }()),
        server_(std::move(server))
    {
    }

    template <typename... Args_>
    static S<Incoming> Create(Args_ &&...args) {
        auto self(Make<Incoming>(std::forward<Args_>(args)...));
        self->self_ = self;
        return self;
    }

    ~Incoming() override {
orc_trace();
        Close();
    }
};

bool Server::Bill(const Buffer &data, bool force) {
    if (cashier_ == nullptr)
        return true;

    const auto amount(cashier_->Bill(data.size()));
    const auto floor(cashier_->Bill(128*1024));

    S<Server> self;

    const auto locked(locked_());
    if (!force && locked->balance_ < amount)
        return false;

    locked->balance_ -= amount;
    ++locked->serial_;

    if (locked->balance_ >= -floor)
        return true;
    std::swap(self, self_);
    return false;
}

task<void> Server::Send(Pipe &pipe, const Buffer &data, bool force) {
    if (Bill(data, force))
        co_return co_await pipe.Send(data);
}

void Server::Send(Pipe &pipe, const Buffer &data) {
    nest_.Hatch([&]() noexcept { return [this, &pipe, data = Beam(data)]() -> task<void> {
        co_return co_await Send(pipe, data, false); }; });
}

task<void> Server::Send(const Buffer &data) {
    co_return co_await Bonded::Send(data);
}

void Server::Commit(const Lock<Locked_> &locked) {
    const auto reveal(Random<32>());
    if (locked->commit_ != locked->reveals_.end())
        locked->commit_->second.second = Timestamp();
    locked->commit_ = locked->reveals_.try_emplace(Hash(reveal), reveal, 0).first;
}

Float Server::Expected(const Lock<Locked_> &locked) {
    auto balance(locked->balance_);
    for (const auto &expected : locked->expected_)
        balance += expected.second;
    return balance;
}

task<void> Server::Invoice(Pipe<Buffer> &pipe, const Socket &destination, const Bytes32 &id, uint64_t serial, const Float &balance, const Bytes32 &commit) {
    Header header{Magic_, id};
    co_await Send(pipe, Datagram(Port_, destination, Tie(header,
        Command(Stamp_, Monotonic()),
        Command(Invoice_, serial, Complement(cashier_->Convert(balance)), cashier_->Tuple(), commit)
    )), true);
}

task<void> Server::Invoice(Pipe<Buffer> &pipe, const Socket &destination, const Bytes32 &id) {
    const auto [serial, balance, commit] = [&]() { const auto locked(locked_());
        return std::make_tuple(locked->serial_, Expected(locked), locked->commit_->first); }();
    co_await Invoice(pipe, destination, id, serial, balance, commit);
}

void Server::Submit(Pipe<Buffer> *pipe, const Socket &source, const Bytes32 &id, const Buffer &data) {
    const auto [
        v, r, s,
        commit,
        issued, nonce,
        lottery, chain,
        amount, ratio,
        start, range,
        funder, recipient,
    window] = Take<
        uint8_t, Brick<32>, Brick<32>,
        Bytes32,
        uint256_t, Bytes32,
        Address, uint256_t,
        uint128_t, uint128_t,
        uint256_t, uint128_t,
        Address, Address,
    Window>(data);

    // XXX: fix Coder and Selector to not require this to Beam
    const Beam receipt(window);

    orc_assert(std::tie(lottery, chain, recipient) == cashier_->Tuple());

    const auto until(start + range);
    const auto now(Timestamp());
    orc_assert(until > now);

    const uint256_t gas(100000);
    const auto [profit, price] = cashier_->Credit(now, start, range, amount, gas);
    if (profit <= 0)
        return;
    static const Float Two128(uint256_t(1) << 128);
    const auto expected(profit * Float(ratio + 1) / Two128);

    using Ticket = Coder<Bytes32, Bytes32, uint256_t, Bytes32, Address, uint256_t, uint128_t, uint128_t, uint256_t, uint128_t, Address, Address, Bytes>;
    static const auto orchid(Hash("Orchid.grab"));
    const auto ticket(Hash(Ticket::Encode(orchid, commit, issued, nonce, lottery, chain, amount, ratio, start, range, funder, recipient, receipt)));
    const Address signer(Recover(Hash(Tie(Strung<std::string>("\x19""Ethereum Signed Message:\n32"), ticket)), v, r, s));

    const auto [reveal, winner] = [&, commit = commit, issued = issued, nonce = nonce, ratio = ratio, expected = expected] {
        const auto locked(locked_());

        orc_assert(issued >= locked->issued_);
        auto &nonces(locked->nonces_);
        orc_assert(nonces.emplace(issued, nonce, signer).second);
        while (nonces.size() > horizon_) {
            const auto oldest(nonces.begin());
            orc_assert(oldest != nonces.end());
            locked->issued_ = std::get<0>(*oldest) + 1;
            nonces.erase(oldest);
        }

        const auto reveal([&]() {
            const auto reveal(locked->reveals_.find(commit));
            orc_assert(reveal != locked->reveals_.end());
            const auto expire(reveal->second.second);
            orc_assert(expire == 0 || reveal->second.second + 60 > now);
            return reveal->second.first;
        }());

        orc_assert(locked->expected_.emplace(ticket, expected).second);
        ++locked->serial_;

        // NOLINTNEXTLINE (clang-analyzer-core.UndefinedBinaryOperatorResult)
        const auto winner(Hash(Tie(reveal, issued, nonce)).skip<16>().num<uint128_t>() <= ratio);
        if (winner && locked->commit_->first == commit)
            Commit(locked);

        return std::make_tuple(reveal, winner);
    }();

    // XXX: the C++ prohibition on automatic capture of a binding name because it isn't a "variable" is ridiculous
    // NOLINTNEXTLINE (clang-analyzer-optin.performance.Padding)
    Spawn([=, price = price, commit = commit, issued = issued, nonce = nonce, v = v, r = r, s = s, amount = amount, ratio = ratio, start = start, range = range, funder = funder, recipient = recipient, reveal = reveal, winner = winner]() noexcept -> task<void> { try {
        const auto valid(co_await cashier_->Check(signer, funder, amount, recipient, receipt));

        {
            const auto locked(locked_());
            const auto expected(locked->expected_.find(ticket));
            orc_assert(expected != locked->expected_.end());
            if (valid)
                locked->balance_ += expected->second;
            else
                ++locked->serial_;
            locked->expected_.erase(expected);
        }

        if (!valid) {
            co_await Invoice(*this, source);
            co_return;
        } else if (!winner)
            co_return;

        std::vector<Bytes32> old;

        static Selector<void,
            Bytes32 /*reveal*/, Bytes32 /*commit*/,
            uint256_t /*issued*/, Bytes32 /*nonce*/,
            uint8_t /*v*/, Bytes32 /*r*/, Bytes32 /*s*/,
            uint128_t /*amount*/, uint128_t /*ratio*/,
            uint256_t /*start*/, uint128_t /*range*/,
            Address /*funder*/, Address /*recipient*/,
            Bytes /*receipt*/, std::vector<Bytes32> /*old*/
        > grab("grab");

        cashier_->Send(grab, gas, price,
            reveal, commit,
            issued, nonce,
            v, r, s,
            amount, ratio,
            start, range,
            funder, recipient,
            receipt, old
        );
    } orc_catch({}) });
}

void Server::Land(Pipe<Buffer> *pipe, const Buffer &data) {
    if (Bill(data, true) && !Datagram(data, [&](const Socket &source, const Socket &destination, const Buffer &data) {
        if (destination != Port_)
            return false;
        if (cashier_ == nullptr)
            return true;

        nest_.Hatch([&]() noexcept { return [this, source, data = Beam(data)]() -> task<void> {
            const auto [header, window] = Take<Header, Window>(data);
            const auto &[magic, id] = header;
            orc_assert(magic == Magic_);

            Scan(window, [&, &id = id](const Buffer &data) { try {
                const auto [command, window] = Take<uint32_t, Window>(data);
                if (command == Submit_)
                    Submit(this, source, id, window);
            } orc_catch({}) });

            co_await Invoice(*this, source, id);
        }; });

        return true;
    })) Send(Inner(), data);
}

void Server::Stop() noexcept {
    self_.reset();
}

void Server::Land(const Buffer &data) {
    if (Bill(data, true))
        Send(*this, data);
}

void Server::Stop(const std::string &error) noexcept {
    orc_insist_(error.empty(), error);
}

Server::Server(S<Origin> origin, S<Cashier> cashier) :
    local_(Certify()),
    origin_(std::move(origin)),
    cashier_(std::move(cashier))
{
    const auto locked(locked_());
    Commit(locked);
}

Server::~Server() {
    orc_trace();
}

task<void> Server::Open(Pipe<Buffer> &pipe) {
    if (cashier_ != nullptr)
        co_await Invoice(pipe, Port_);
}

task<void> Server::Shut() noexcept {
    co_await nest_.Shut();
    *co_await Parallel(Bonded::Shut(), Sunken::Shut());
}

task<std::string> Server::Respond(const std::string &offer, std::vector<std::string> ice) {
    auto incoming(Incoming::Create(self_, origin_, local_, std::move(ice)));
    auto answer(co_await incoming->Answer(offer));
    co_return answer;
    co_return Filter(true, answer);
}

std::string Filter(bool answer, const std::string &serialized) {
    webrtc::JsepSessionDescription jsep(answer ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer);
    webrtc::SdpParseError error;
    orc_assert(webrtc::SdpDeserialize(serialized, &jsep, &error));

    auto description(jsep.description());
    orc_assert(description != nullptr);

    std::vector<cricket::Candidate> privates;

    for (size_t i(0); ; ++i) {
        auto ices(jsep.candidates(i));
        if (ices == nullptr)
            break;
        for (size_t i(0), e(ices->count()); i != e; ++i) {
            auto ice(ices->at(i));
            orc_assert(ice != nullptr);
            const auto &candidate(ice->candidate());
            if (candidate.address().IsPrivateIP())
                privates.push_back(candidate);
        }
    }

    for (auto &p : privates)
        p.set_transport_name("0");
    orc_assert(jsep.RemoveCandidates(privates) == privates.size());

    return webrtc::SdpSerialize(jsep);
}

}
