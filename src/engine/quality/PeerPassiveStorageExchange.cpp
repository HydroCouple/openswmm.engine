// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "PeerPassiveStorageExchange.hpp"
#include "PassiveStorageLegAdmission.hpp"
#include "QualityRouting.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../2d/data/SurfaceStateData.hpp"
#include "../2d/coupling/NodeCoupling.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <set>
#include <random>

namespace openswmm::quality {
namespace {
void require(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
std::uint64_t nonce() {
    // A fresh host process must not re-create an archived capability epoch.
    // Entropy failure refuses attachment; there is no deterministic fallback.
    // These native session labels are not cryptographic remote authentication.
    static std::atomic<std::uint64_t> next{[] {
        std::random_device entropy;
        return std::uniform_int_distribution<std::uint64_t>(1,
            std::numeric_limits<std::uint64_t>::max()-1)(entropy);
    }()};
    auto value=next.load();
    do { require(value!=std::numeric_limits<std::uint64_t>::max(),"Peer session namespace exhausted."); }
    while(!next.compare_exchange_weak(value,value+1));
    return value;
}
std::uint64_t recoveryNonce() {
    // Consult host entropy again on each recovery, including after fork without
    // exec. Never inherit the parent's initialized process-local counter alone.
    std::random_device entropy;
    return std::uniform_int_distribution<std::uint64_t>(1,
        std::numeric_limits<std::uint64_t>::max())(entropy);
}
std::size_t capacity(std::size_t value) {
    require(value>0 && value<=65536,"Invalid peer journal capacity.");return value;
}
}
struct PeerRecipientBinding {
    const SimulationContext* surface;
    PassiveStorageExchange::PeerIdentity identity;
    bool claimed=false,alive=false,failed=false;
};
struct PeerFundingToken {
    enum State { ISSUED, OPEN, SEALED, PREPARED, CONFIRMED, RETIRED, FAILED } state=ISSUED;
    PeerFundingView issued;
    std::shared_ptr<PeerRecipientBinding> recipient;
    PeerFundingToken(PeerFundingView view,std::shared_ptr<PeerRecipientBinding> binding)
        :issued(std::move(view)),recipient(std::move(binding)){}
};
PeerFundingOffer::PeerFundingOffer(std::shared_ptr<PeerFundingToken> token):view_(token->issued),token_(std::move(token)){}
PeerFundingReply::PeerFundingReply(const PeerFundingOffer& offer):view_(offer.view_),token_(offer.token_){}
void PeerPassiveStorageFunding::validateProfile(const SimulationContext& ctx) {
    PassiveStorageExchange book(PassiveStorageArithmetic::RETAIN_EXACT_SLICES,
        PassiveStorageLaw::CHANGING_VOLUME,PassiveStorageWaterArithmetic::RETAIN_AGGREGATES);
    book.validate(ctx);
}

PeerPassiveStorageFunding::PeerPassiveStorageFunding(SimulationContext& ctx,std::uint64_t issuer,std::uint64_t recipient,
        std::vector<PassiveStorageExchange::HydraulicPoint> points,double factor,std::size_t limit)
    :context_(&ctx),identity_{issuer,recipient,nonce()},points_(std::move(points)),factor_(factor),capacity_(capacity(limit)) {
    require(issuer && recipient && issuer!=recipient && std::isfinite(factor) && factor>0 &&
        !points_.empty() && points_.size()<=4096,"Invalid peer funding identity or topology.");
    std::vector<int> nodes;
    std::set<int> mapped;
    for(int n=0;n<ctx.n_nodes();++n)nodes.push_back(n);
    for(std::size_t k=0;k<points_.size();++k) {
        require(points_[k].point==k && points_[k].cell>=0 && points_[k].node>=0 && points_[k].node<ctx.n_nodes() &&
            points_[k].exchange_si==0,"Invalid ordered peer point map.");
        mapped.insert(points_[k].node);
    }
    require(mapped.size()==nodes.size(),"Peer closed profile requires every native node to be mapped.");
    book_.bindChangingVolumePools(ctx,nodes);book_.peer_binding_=identity_;
}
PeerPassiveStorageFunding::~PeerPassiveStorageFunding(){if(unresolved())fail("Peer funding owner closed with unresolved work.");}
bool PeerPassiveStorageFunding::failed() const noexcept {
    return failure_ || book_.failed() || (recipient_ && recipient_->failed) || (token_ && token_->state==PeerFundingToken::FAILED);
}
bool PeerPassiveStorageFunding::unresolved() const noexcept {return book_.funding_.has_value() || (token_ && token_->state!=PeerFundingToken::RETIRED);}
std::array<double,2> PeerPassiveStorageFunding::lastWindow() const noexcept {
    if(book_.water_publications_.empty())return {0,0};
    const auto& p=book_.water_publications_.back().publication;return {p.batch_start_ms,p.batch_end_ms};
}
void PeerPassiveStorageFunding::ready() const {require(!failed(),"Peer funding session is terminal.");}
void PeerPassiveStorageFunding::fail(const char* reason) noexcept {
    if(!failure_)failure_=reason;if(recipient_)recipient_->failed=true;if(token_)token_->state=PeerFundingToken::FAILED;
}
std::unique_ptr<PeerFundingRecipient> PeerPassiveStorageFunding::bindRecipient(const SimulationContext& surface) {
    ready();require(!recipient_ && &surface!=context_,"Peer requires one distinct native recipient context.");
    auto binding=std::make_shared<PeerRecipientBinding>(PeerRecipientBinding{&surface,identity_});
    auto capability=std::unique_ptr<PeerFundingRecipient>(new PeerFundingRecipient(binding));
    recipient_=std::move(binding);return capability;
}
void PeerPassiveStorageFunding::freeze(double dt){ready();require(!unresolved(),"Peer funding remains unresolved.");book_.freezeDelivery(*context_,dt);}
void PeerPassiveStorageFunding::observeDrain(){ready();book_.observeDeliveryDrain(*context_);}
void PeerPassiveStorageFunding::receive(QualitySolver& solver,double dt){ready();require(!unresolved(),"Peer funding remains unresolved.");solver.execute(*context_,dt,book_);}
std::unique_ptr<PeerFundingOffer> PeerPassiveStorageFunding::issue(Phase phase,double start,double end) {
    ready();require(!unresolved() && recipient_ && recipient_->claimed && recipient_->alive,
        "Peer funding requires its registered recipient and cannot be reissued or retried.");
    try {
        book_.grantFunding(*context_,book_.last_id_+1,phase,start,end,capacity_-committed_legs_);
        PeerFundingView view;view.allowance=*book_.funding_;view.points=points_;view.si_to_native=factor_;
        view.nodes=context_->node_names.names();
        const auto& n=context_->nodes;view.head=n.head;view.volume=n.volume;view.depth=n.depth;
        view.full_volume=n.full_volume;view.rpt_full_volume=n.rpt_full_volume;view.ponded_area=n.ponded_area;
        token_=std::make_shared<PeerFundingToken>(std::move(view),recipient_);reply_.reset();
        return std::unique_ptr<PeerFundingOffer>(new PeerFundingOffer(token_));
    }catch(...){if(book_.funding_)fail("Peer offer could not be delivered; grant retained.");throw;}
}
void PeerPassiveStorageFunding::accept(std::unique_ptr<PeerFundingReply> reply) {
    ready();
    if(reply_) {fail("Peer reply was already returned; first diagnostic retained.");throw std::invalid_argument("Duplicate peer reply.");}
    reply_=std::move(reply);
    try {
        require(token_ && token_->state==PeerFundingToken::SEALED && reply_ && reply_->token_==token_ &&
            token_->recipient==recipient_ && reply_->view_==token_->issued && reply_->view_.allowance.peer==identity_ && book_.funding_ &&
            !book_.funding_->peer_reported,"Peer reply identity, state or offer differs.");
        const auto& proof=reply_->proof_;const auto& grant=token_->issued.allowance;
        require(proof.si_to_native==factor_ && proof.points.size()==points_.size() && proof.legs.size()<=grant.leg_limit &&
            reply_->spent_.size()==grant.pools.size(),"Peer proof extent or conversion differs.");
        for(std::size_t k=0;k<points_.size();++k)
            require(proof.points[k].point==points_[k].point && proof.points[k].node==points_[k].node &&
                proof.points[k].cell==points_[k].cell,"Peer proof point mapping differs.");
        auto pools=book_.funding_->pools;
        std::vector<NativeAmountSum> reported(pools.size());
        for(const auto& leg:proof.legs) {
            require(leg.node>=0 && std::size_t(leg.node)<reported.size(),"Peer proof names an unknown native node.");
            if(!leg.drain)reported[leg.node].add(leg.mass_native);
        }
        for(std::size_t k=0;k<pools.size();++k) {
            require(pools[k].node==int(k) && reported[k]==reply_->spent_[k] && pools[k].granted.atLeast(reported[k]),
                "Peer reported spend exceeds issued authority.");
            pools[k].spent=reported[k];
        }
        // This is received evidence from the native consumer, never a replay of
        // spendFunding on the issuer as if it had witnessed each cell admission.
        auto received=proof;book_.peer_received_=std::move(received);
        book_.funding_->pools.swap(pools);book_.funding_->peer_reported=true;
        if(!reply_->rows_.empty())book_.reserveAuthenticated(*context_,grant.receipt,reply_->rows_,grant.phase,grant.start_ms,grant.end_ms,proof);
        else require(proof.legs.empty(),"Peer proof omitted gross rows.");
        book_.beginWaterPublication(*context_,proof,grant.phase,grant.start_ms,grant.end_ms,(grant.end_ms-grant.start_ms)/1000);
        token_->state=PeerFundingToken::PREPARED;
    }catch(...){fail("Peer reply refused; original encumbrance and reported work retained.");throw;}
}
void PeerPassiveStorageFunding::validateWaterBooking(const std::vector<double>& queue,const std::vector<double>& direct,double remaining) const {
    ready();
    require(token_ && token_->state==PeerFundingToken::PREPARED && book_.pending_water_ &&
        queue==book_.pending_water_->after && direct.size()==queue.size() &&
        remaining==book_.pending_water_->remaining_after,"Peer returned water booking differs from prepared publication.");
    for(double value:direct)require(value==0,"Peer returned an undelivered direct water volume.");
}
std::unique_ptr<PeerFundingAcknowledgement> PeerPassiveStorageFunding::confirmWater() {
    ready();
    try {
        require(token_ && token_->state==PeerFundingToken::PREPARED,"Peer confirmation lacks a prepared reply.");
        auto ack=std::unique_ptr<PeerFundingAcknowledgement>(new PeerFundingAcknowledgement(token_));
        book_.finishWaterPublication(*context_);committed_legs_+=reply_->proof_.legs.size();
        token_->state=PeerFundingToken::CONFIRMED;return ack;
    }catch(...){fail("Peer water confirmation refused; work retained.");throw;}
}

PeerPassiveStorageProducer::PeerPassiveStorageProducer(const SimulationContext& ctx,const twoD::SurfaceStateData& state,
        std::unique_ptr<PeerFundingRecipient> recipient,double factor,std::size_t limit)
    :context_(&ctx),state_(&state),factor_(factor),legs_(capacity(limit)) {
    require(recipient && recipient->binding_ && recipient->binding_->surface==&ctx &&
        !recipient->binding_->claimed && std::isfinite(factor) && factor>0,"Invalid peer producer capability.");
    recipient_=std::move(recipient->binding_);identity_=recipient_->identity;
    recipient_->claimed=recipient_->alive=true;
}
PeerPassiveStorageProducer::~PeerPassiveStorageProducer(){if(open_ || awaiting_ack_)fail("Peer producer closed with unresolved work.");if(recipient_)recipient_->alive=false;}
bool PeerPassiveStorageProducer::failed() const noexcept {return failure_ || (recipient_ && recipient_->failed) || (token_ && token_->state==PeerFundingToken::FAILED);}
void PeerPassiveStorageProducer::fail(const char* reason) noexcept {
    if(!failure_)failure_=reason;if(recipient_)recipient_->failed=true;if(token_)token_->state=PeerFundingToken::FAILED;
}
void PeerPassiveStorageProducer::begin(std::unique_ptr<PeerFundingOffer> offer) {
    require(!failed() && !open_ && !awaiting_ack_,"Peer producer is not at a ready boundary.");
    offer_=std::move(offer);
    try {
        require(offer_ && offer_->token_ && offer_->token_->state==PeerFundingToken::ISSUED &&
            offer_->token_->recipient==recipient_,"Peer offer is missing, already consumed or belongs to another recipient.");
        token_=offer_->token_;const auto& view=offer_->view_;const auto& a=view.allowance;const auto& ctx=*context_;const auto& state=*state_;
        require(view==token_->issued && a.peer==identity_ && a.ordinal==last_ordinal_+1 && ctx.elapsed_ms==a.capture_ms &&
            view.si_to_native==factor_ && a.leg_limit<=legs_.size()-count_,"Peer offer session, ordinal, limits or conversion differs.");
        require(ctx.n_pollutants()==1 && ctx.pollutants.unit_declaration_valid.at(0)==1 &&
            a.pollutant==ctx.pollutant_names.names()[0] && a.units==ctx.pollutants.units.at(0) &&
            state.transport.n_species==1 && state.transport.n_pollut==1 && state.node_row_conc &&
            state.node_row_conc->size()==std::size_t(ctx.n_nodes()) && state.nodes_1d==&ctx.nodes && state.node_coupling &&
            state.node_coupling->size()==view.points.size(),"Peer native species or surface owner differs.");
        const auto& n=ctx.nodes;
        require(ctx.node_names.names()==view.nodes && n.head==view.head && n.volume==view.volume && n.depth==view.depth && n.full_volume==view.full_volume &&
            n.rpt_full_volume==view.rpt_full_volume && n.ponded_area==view.ponded_area,"Peer published native water boundary differs.");
        for(std::size_t k=0;k<view.points.size();++k) {
            const auto& p=view.points[k];const auto& cp=state.node_coupling->at(k);
            require(p.point==k && cp.cell_idx==p.cell && cp.node_idx==p.node && !cp.is_outfall &&
                p.cell>=0 && std::size_t(p.cell)<state.volume.size() &&
                std::size_t(p.cell)<state.transport.cell_mass.size(),"Peer surface point map differs.");
        }
        for(const auto& pool:a.pools)
            require(pool.node>=0 && pool.node<ctx.n_nodes() && n.type.at(pool.node)==NodeType::STORAGE &&
                n.conc.at(pool.node)==pool.published_concentration && state.node_row_conc->at(pool.node)==pool.published_concentration,
                "Peer native published concentration differs.");
        spent_.assign(a.pools.size(),{});first_=count_;reserved_=false;open_=true;token_->state=PeerFundingToken::OPEN;
    }catch(...){fail("Peer offer refused before producer effects.");throw;}
}
bool PeerPassiveStorageProducer::admit(std::size_t point,int cell,int node,double water,double mass,double before,
        double volume,double offset,double duration,double withdrawal) noexcept {
    if(failed())return false;
    auto reject=[&](const char* reason){fail(reason);return false;};
    if(!offer_)return reject("Peer producer has no issued offer.");
    const auto& a=offer_->view_.allowance;
    attempted_=passiveStorageAttempt(point,cell,node,water,mass,before,volume,offset,duration,withdrawal,
        factor_,a.start_ms,a.end_ms,a.phase);
    if(!open_ || reserved_ || !token_ || token_->state!=PeerFundingToken::OPEN ||
        context_->elapsed_ms!=a.capture_ms || point>=offer_->view_.points.size())
        return reject("Peer admission has no open single-use window.");
    const auto& mapping=offer_->view_.points[point];
    if(mapping.cell!=cell || mapping.node!=node || count_==legs_.size() || count_-first_==a.leg_limit)
        return reject("Peer admission map or journal limit differs.");
    if(cell<0 || std::size_t(cell)>=state_->volume.size() || std::size_t(cell)>=state_->transport.cell_mass.size() ||
        state_->volume[cell]!=volume || state_->transport.cell_mass[cell]!=before)
        return reject("Peer admission operands differ from its bound native cell.");
    const auto it=std::find_if(a.pools.begin(),a.pools.end(),[&](const auto& p){return p.node==node;});
    if(it==a.pools.end())return reject("Peer admission node is unowned.");
    attempted_.sample=it->published_concentration;
    if(const auto* reason=passiveStorageLegError(attempted_,water))return reject(reason);
    if(water<0) {
        const auto k=std::size_t(it-a.pools.begin());auto next=spent_[k];
        if(!next.tryAdd(attempted_.mass_native) || !it->granted.atLeast(next))return reject("Peer spill exceeds its issued pre-credit allowance.");
        spent_[k]=next;
    }
    legs_[count_++]=attempted_;reserved_=true;return true;
}
void PeerPassiveStorageProducer::applied(double after,double volume) noexcept {
    if(!reserved_ || count_==0){fail("Peer producer has no admitted leg to apply.");return;}
    auto& leg=legs_[count_-1];leg.applied=true;reserved_=false;
    if(leg.after!=after || leg.volume_after!=volume || state_->transport.cell_mass[leg.cell]!=after ||
        state_->volume[leg.cell]!=volume)fail("Peer native write differs from admission.");
}
std::unique_ptr<PeerFundingReply> PeerPassiveStorageProducer::seal(const std::vector<double>& exchange) {
    require(!failed(),"Peer producer is terminal.");
    try {
        require(open_ && !reserved_ && token_->state==PeerFundingToken::OPEN &&
            context_->elapsed_ms==offer_->view_.allowance.capture_ms &&
            exchange.size()==offer_->view_.points.size(),"Peer producer has no complete hydraulic proof.");
        auto reply=std::unique_ptr<PeerFundingReply>(new PeerFundingReply(*offer_));auto& proof=reply->proof_;
        proof.si_to_native=factor_;proof.legs.assign(legs_.begin()+first_,legs_.begin()+count_);reply->spent_=spent_;
        std::vector<std::size_t> head(exchange.size(),proof.legs.size()),next(proof.legs.size());
        for(std::size_t i=proof.legs.size();i>0;) {
            --i;const auto point=proof.legs[i].point;
            require(point<head.size(),"Peer journal point is outside its issued map.");
            next[i]=head[point];head[point]=i;
        }
        for(std::size_t k=0;k<exchange.size();++k) {
            auto point=offer_->view_.points[k];point.exchange_si=exchange[k];proof.points.push_back(point);
            PassiveStorageExchange::Row row{k,point.node,offer_->view_.allowance.pollutant,offer_->view_.allowance.units};
            NativeAmountSum dw,sw,dm,sm;double actual=0;
            for(std::size_t i=head[k];i<proof.legs.size();i=next[i]) {
                const auto& leg=proof.legs[i];
                require(leg.applied,"Peer producer has an unapplied leg.");actual+=leg.drain?leg.water_si:-leg.water_si;
                (leg.drain?dw:sw).add(leg.water_native);(leg.drain?dm:sm).add(leg.mass_native);
            }
            require(actual==exchange[k],"Peer gross legs differ from actual hydraulic point booking.");
            if(!dw.zero() || !sw.zero()) {
                row.setWaterAmount(true,dw);row.setWaterAmount(false,sw);row.setMaterialAmount(true,dm);row.setMaterialAmount(false,sm);
                reply->rows_.push_back(std::move(row));
            }
        }
        token_->state=PeerFundingToken::SEALED;open_=false;awaiting_ack_=true;return reply;
    }catch(...){fail("Peer producer cannot seal; applied journal retained.");throw;}
}
void PeerPassiveStorageProducer::acknowledge(std::unique_ptr<PeerFundingAcknowledgement> ack) {
    try {
        require(!failed() && awaiting_ack_ && ack && ack->token_==token_ && token_->state==PeerFundingToken::CONFIRMED,
            "Peer acknowledgement is missing, stale or belongs to another session.");
        last_ordinal_=offer_->view_.allowance.ordinal;awaiting_ack_=false;token_->state=PeerFundingToken::RETIRED;
        offer_.reset();
    }catch(...){fail("Peer acknowledgement refused; applied journal retained.");throw;}
}

namespace {
using PeerArchive = detail::CompletedSourceArchive;
constexpr std::size_t maxPeerBytes = 64 * 1024 * 1024;
const std::string peerMagic = "OpenSWMM acknowledged network/surface storage owners";
std::uint32_t peerChecksum(const std::string& bytes) {
    std::uint32_t value = ~std::uint32_t(0);
    for (unsigned char c : bytes) {
        value ^= c;
        for (int bit=0;bit<8;++bit) value=(value>>1)^((value&1)?0xedb88320u:0u);
    }
    return ~value;
}
}
struct PeerPassiveStorageCheckpoint::Journal {
    std::vector<PeerPassiveStorageProducer::Leg> legs;
    std::size_t first = 0;
    std::uint64_t ordinal = 0;
    std::vector<NativeAmountSum> spent;
    PeerPassiveStorageProducer::Leg attempted;
};
PeerPassiveStorageCheckpoint::Journal
PeerPassiveStorageCheckpoint::journal(const PeerPassiveStorageFunding& f) {
    Journal out;
    const auto& book=f.book_;
    std::size_t receipt=0;
    for (const auto& w:book.water_publications_) {
        require(w.si_to_native==f.factor_ && w.points.size()==f.points_.size(),
            "Peer checkpoint water publication conversion/extent differs.");
        for (std::size_t k=0;k<w.points.size();++k)
            require(w.points[k].point==f.points_[k].point && w.points[k].cell==f.points_[k].cell &&
                w.points[k].node==f.points_[k].node,"Peer checkpoint publication map differs.");
        out.first=out.legs.size();out.spent.assign(book.initial_pools_.size(),{});
        if (w.receipt) {
            require(receipt<book.receipts_.size(),"Peer checkpoint publication lacks its receipt.");
            const auto& r=book.receipts_[receipt++];
            require(r.id==w.receipt && r.authenticated && r.proof.si_to_native==f.factor_ &&
                r.proof.points==w.points && !r.proof.legs.empty() &&
                r.proof.legs.size()<=f.capacity_-out.legs.size(),"Peer checkpoint proof/journal extent differs.");
            for (const auto& leg:r.proof.legs) {
                require(leg.applied && leg.point<f.points_.size() && leg.cell==f.points_[leg.point].cell &&
                    leg.node==f.points_[leg.point].node && leg.node>=0 && std::size_t(leg.node)<out.spent.size(),
                    "Peer checkpoint journal map differs.");
                out.legs.push_back(leg);
                if (!leg.drain) out.spent[leg.node].add(leg.mass_native);
            }
        }
        ++out.ordinal;
    }
    require(receipt==book.receipts_.size(),"Peer checkpoint has unpublished material receipts.");
    if (!out.legs.empty()) { out.attempted=out.legs.back();out.attempted.applied=false; }
    return out;
}
void PeerPassiveStorageCheckpoint::surface(const PeerPassiveStorageFunding& f,
        const SimulationContext& network,const SimulationContext& surface,const twoD::SurfaceStateData& state) {
    require(&network!=&surface && network.elapsed_ms==surface.elapsed_ms &&
        network.node_names.names()==f.book_.node_ids_ &&
        network.pollutant_names.names()==f.book_.pollutant_ids_ &&
        network.pollutants.units==f.context_->pollutants.units &&
        network.pollutants.unit_declaration_valid==f.context_->pollutants.unit_declaration_valid &&
        network.nodes.type==f.context_->nodes.type &&
        network.node_names.names()==surface.node_names.names() &&
        network.pollutant_names.names()==surface.pollutant_names.names() &&
        network.pollutants.units==surface.pollutants.units &&
        network.nodes.type==surface.nodes.type && surface.n_pollutants()==1 &&
        surface.pollutants.unit_declaration_valid.at(0)==1 && network.nodes.conc==surface.nodes.conc &&
        state.transport.n_species==1 && state.transport.n_pollut==1 && state.nodes_1d==&surface.nodes &&
        state.node_row_conc && state.node_row_conc->size()==std::size_t(surface.n_nodes()) &&
        state.node_coupling && state.node_coupling->size()==f.points_.size() && !state.live_passive_exchange,
        "Peer checkpoint requires distinct accepted native owners and a matching declared surface basis.");
    for (std::size_t k=0;k<f.points_.size();++k) {
        const auto& point=state.node_coupling->at(k);const auto& saved=f.points_[k];
        require(saved.point==k && point.cell_idx==saved.cell && point.node_idx==saved.node &&
            !point.is_outfall && saved.cell>=0 && std::size_t(saved.cell)<state.volume.size() &&
            std::size_t(saved.cell)<state.transport.cell_mass.size(),"Peer checkpoint actual surface map differs.");
    }
}
void PeerPassiveStorageCheckpoint::boundary(const PeerPassiveStorageFunding& f,const PeerPassiveStorageProducer& p) {
    require(!f.failed() && !f.unresolved() && !p.failed() && !p.open_ && !p.reserved_ && !p.awaiting_ack_ &&
        !p.offer_ && f.recipient_ && f.recipient_==p.recipient_ && f.recipient_->claimed && f.recipient_->alive &&
        f.recipient_->surface==p.context_ && f.identity_==p.identity_ && f.factor_==p.factor_ &&
        f.capacity_==p.legs_.size() && f.book_.peer_binding_==f.identity_,
        "Peer checkpoint requires healthy acknowledged paired owners.");
    // A recovered accepted boundary has no old capsules. Otherwise both owners
    // must still share their actual retired token; CONFIRMED without ACK refuses.
    require((!f.token_ && !p.token_ && !f.reply_) ||
        (f.token_ && f.token_==p.token_ && f.token_->state==PeerFundingToken::RETIRED &&
         f.token_->recipient==f.recipient_ && f.reply_ && f.reply_->token_==f.token_),
        "Peer checkpoint has an unresolved or mismatched native capsule.");
    surface(f,*f.context_,*p.context_,*p.state_);
    const auto j=journal(f);
    require(f.committed_legs_==j.legs.size() && p.count_==j.legs.size() && p.first_==j.first &&
        p.last_ordinal_==j.ordinal && p.spent_==j.spent && p.attempted_==j.attempted &&
        std::equal(j.legs.begin(),j.legs.end(),p.legs_.begin()),
        "Peer checkpoint producer journal/counters differ from acknowledged history.");
}
void PeerPassiveStorageCheckpoint::fresh(const PeerPassiveStorageFunding& f,const PeerPassiveStorageProducer& p) {
    boundary(f,p);
    require(f.context_->elapsed_ms==0 && p.context_->elapsed_ms==0 && f.book_.receipts_.empty() &&
        f.book_.deliveries_.empty() && f.book_.water_publications_.empty() && !f.token_ && !f.reply_,
        "Peer checkpoint restore requires fresh initial enrolled owners.");
    // Includes the native frozen/prepared/seal checks, without weakening the
    // standalone API's unconditional peer refusal.
    f.book_.checkpointSealed(*f.context_);
}
std::string PeerPassiveStorageCheckpoint::save(const PeerPassiveStorageFunding& f,const PeerPassiveStorageProducer& p) {
    boundary(f,p);
    auto book=f.book_.checkpointSealed(*f.context_);
    PeerArchive payload;
    auto identity=f.identity_;auto factor=f.factor_;std::uint64_t capacity=f.capacity_,count=f.points_.size();
    payload(identity.issuer,identity.recipient,identity.session,factor,capacity,count);
    for (auto point:f.points_) payload(point.point,point.cell,point.node,point.exchange_si);
    payload(book);
    auto bytes=payload.bytes();auto magic=peerMagic;
    std::uint32_t version=1,endian=0x01020304,word=sizeof(std::size_t),crc=peerChecksum(bytes);
    PeerArchive envelope;envelope(magic,version,endian,word,crc,bytes);
    require(envelope.bytes().size()<=maxPeerBytes,"Peer checkpoint exceeds its byte limit.");
    return envelope.bytes();
}
struct PeerPassiveStorageCheckpoint::Prepared::State {
    std::unique_ptr<PeerPassiveStorageFunding>* issuer_slot;
    std::unique_ptr<PeerPassiveStorageProducer>* producer_slot;
    const PeerPassiveStorageFunding* original_issuer;
    const PeerPassiveStorageProducer* original_producer;
    PeerPassiveStorageFunding::Identity archived_identity;
    bool authorized = false;
    std::unique_ptr<PeerPassiveStorageFunding> issuer;
    std::unique_ptr<PeerPassiveStorageProducer> producer; // destroyed before its issuer
};
PeerPassiveStorageCheckpoint::Prepared::Prepared(std::unique_ptr<State> state):state_(std::move(state)){}
PeerPassiveStorageCheckpoint::Prepared::Prepared(Prepared&&) noexcept=default;
PeerPassiveStorageCheckpoint::Prepared& PeerPassiveStorageCheckpoint::Prepared::operator=(Prepared&&) noexcept=default;
PeerPassiveStorageCheckpoint::Prepared::~Prepared()=default;
std::uint64_t PeerPassiveStorageCheckpoint::recoveryIdentity() { return recoveryNonce(); }
const PeerPassiveStorageProducer& PeerPassiveStorageCheckpoint::Prepared::producer() const {
    require(bool(state_),"Peer checkpoint candidate was consumed.");return *state_->producer;
}
const PeerPassiveStorageFunding& PeerPassiveStorageCheckpoint::Prepared::funding() const {
    require(bool(state_),"Peer checkpoint candidate was consumed.");return *state_->issuer;
}
void PeerPassiveStorageCheckpoint::Prepared::rekey(std::uint64_t issuer,std::uint64_t recipient,
        std::uint64_t archived_issuer,std::uint64_t archived_recipient) {
    require(state_ && issuer && recipient && issuer!=recipient,"Invalid recovered peer identities.");
    require(state_->archived_identity.issuer==archived_issuer && state_->archived_identity.recipient==archived_recipient,
        "Material archived role provenance differs from its peer owners.");
    auto& f=*state_->issuer;auto& p=*state_->producer;
    f.identity_.issuer=issuer;f.identity_.recipient=recipient;f.book_.peer_binding_=f.identity_;
    f.recipient_->identity=f.identity_;p.identity_=f.identity_;
}
void PeerPassiveStorageCheckpoint::Prepared::validateTarget() const {
    if (state_) state_->authorized=false;
    require(state_ && state_->issuer_slot->get()==state_->original_issuer &&
        state_->producer_slot->get()==state_->original_producer,"Peer checkpoint target owners were replaced or consumed.");
    PeerPassiveStorageCheckpoint::fresh(**state_->issuer_slot,**state_->producer_slot);
    state_->authorized=true;
}
void PeerPassiveStorageCheckpoint::Prepared::install() noexcept {
    if (!state_ || !state_->authorized || state_->issuer_slot->get()!=state_->original_issuer ||
        state_->producer_slot->get()!=state_->original_producer) return;
    state_->issuer_slot->swap(state_->issuer);state_->producer_slot->swap(state_->producer);
    state_.reset();
}
PeerPassiveStorageCheckpoint::Prepared PeerPassiveStorageCheckpoint::prepare(
        std::unique_ptr<PeerPassiveStorageFunding>& issuer,std::unique_ptr<PeerPassiveStorageProducer>& producer,
        const SimulationContext& incoming_network,const SimulationContext& incoming_surface,
        const twoD::SurfaceStateData& incoming_state,const std::string& bytes) {
    require(issuer && producer,"Peer checkpoint target lacks its enrolled owners.");
    fresh(*issuer,*producer);
    require(!bytes.empty() && bytes.size()<=maxPeerBytes,"Peer checkpoint exceeds its byte limit.");
    PeerArchive envelope(bytes);
    std::string magic,payload;std::uint32_t version,endian,word,crc;
    envelope(magic,version,endian,word,crc,payload);envelope.finish();
    require(magic==peerMagic && version==1 && endian==0x01020304 && word==sizeof(std::size_t) &&
        crc==peerChecksum(payload),"Peer checkpoint format/version/checksum differs.");
    PeerArchive archive(std::move(payload));
    PeerPassiveStorageFunding::Identity old;
    double factor;std::uint64_t limit,count;
    archive(old.issuer,old.recipient,old.session,factor,limit,count);
    require(old.issuer && old.recipient && old.issuer!=old.recipient && old.session &&
        factor==issuer->factor_ && limit==issuer->capacity_ && count==issuer->points_.size(),
        "Peer checkpoint identity/conversion/capacity differs.");
    for (const auto& expected:issuer->points_) {
        PassiveStorageExchange::HydraulicPoint point;
        archive(point.point,point.cell,point.node,point.exchange_si);
        require(point==expected,"Peer checkpoint target point map differs.");
    }
    std::string book;archive(book);archive.finish();
    surface(*issuer,incoming_network,incoming_surface,incoming_state);
    auto state=std::make_unique<Prepared::State>();
    state->archived_identity=old;
    state->issuer_slot=&issuer;state->producer_slot=&producer;
    state->original_issuer=issuer.get();state->original_producer=producer.get();
    state->issuer=std::make_unique<PeerPassiveStorageFunding>(*issuer->context_,issuer->identity_.issuer,
        issuer->identity_.recipient,issuer->points_,issuer->factor_,issuer->capacity_);
    auto& f=*state->issuer;
    do { f.identity_.session=recoveryNonce(); }
    while (f.identity_.session==old.session || f.identity_.session==issuer->identity_.session);
    f.book_.peer_binding_=f.identity_;
    state->producer=std::make_unique<PeerPassiveStorageProducer>(*producer->context_,*producer->state_,
        f.bindRecipient(*producer->context_),f.factor_,f.capacity_);
    f.book_.restoreSealed(incoming_network,book,&issuer->book_.initial_pools_);
    const auto j=journal(f);
    auto& p=*state->producer;
    std::copy(j.legs.begin(),j.legs.end(),p.legs_.begin());p.count_=j.legs.size();p.first_=j.first;
    p.last_ordinal_=j.ordinal;p.spent_=j.spent;p.attempted_=j.attempted;f.committed_legs_=j.legs.size();
    // The enclosing transaction owns numerical publication. Never keep pointers
    // to its detached validation contexts, and never reconstruct retired capsules.
    f.book_.owner_=f.context_;
    return Prepared(std::move(state));
}
}
