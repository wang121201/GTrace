#pragma once
// Host representation only. Logical IDs are never recycled or renumbered.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace GTSim {
#ifndef TILEGEN_CTA_PAGED_SLOTS
#define TILEGEN_CTA_PAGED_SLOTS 1
#endif
#if TILEGEN_CTA_PAGED_SLOTS
template<class T> class CtaSparseSlots {
    static constexpr std::size_t cell_bits_=6,page_bits_=6,group_bits_=8;
    static constexpr std::size_t cells_=std::size_t(1)<<cell_bits_;
    static constexpr std::size_t pages_=std::size_t(1)<<page_bits_;
    static constexpr std::size_t groups_=std::size_t(1)<<group_bits_;
    static constexpr std::size_t block_bits_=cell_bits_+page_bits_+group_bits_;
    static constexpr std::size_t max_direct_roots_=std::size_t(1)<<16;
    struct Cell {alignas(T) std::byte storage[sizeof(T)];};
    struct Page {
        std::uint64_t present=0;
        std::array<Cell,cells_> cells;
        T* address(std::size_t i){return reinterpret_cast<T*>(cells[i].storage);}
        T& get(std::size_t i){return *std::launder(address(i));}
        const T& get(std::size_t i)const{return *std::launder(reinterpret_cast<const T*>(cells[i].storage));}
        T& create(std::size_t i,const T& value) {
            T* p=address(i);
            if constexpr(std::is_copy_constructible_v<T>)std::construct_at(p,value);
            else std::construct_at(p);
            // A failed constructor leaves no presence bit and no owned object.
            present|=std::uint64_t(1)<<i;return *std::launder(p);
        }
        void erase(std::size_t i) {
            present&=~(std::uint64_t(1)<<i);std::destroy_at(std::addressof(get(i)));
        }
        ~Page(){while(present){const auto i=std::countr_zero(present);erase(i);}}
    };
    struct Group {std::array<std::unique_ptr<Page>,pages_> pages;std::size_t count=0;};
    struct Block {std::array<std::unique_ptr<Group>,groups_> groups;std::size_t count=0;};
    std::size_t domain_=0,peak_=0,entries_=0,allocated_pages_=0,allocated_groups_=0,allocated_blocks_=0;
    // The bounded root costs at most 65536 pointers, never one pointer per ID.
    // It is allocated only on first insertion. All lower directories are sparse.
    std::vector<std::unique_ptr<Block>> root_;
    // Huge size_t domains use a sparse ROOT fallback, never a per-entry map.
    std::unordered_map<std::size_t,std::unique_ptr<Block>> sparse_root_;
    T default_{};
    static std::size_t roots_for(std::size_t n){return n?((n-1)>>block_bits_)+1:0;}
    bool direct_root()const{return roots_for(domain_)<=max_direct_roots_;}
    void bounds(std::size_t i)const{if(i>=domain_)throw std::out_of_range("CTA sparse logical ID");}
    Block* find_block(std::size_t id)const {
        if(direct_root())return id<root_.size()?root_[id].get():nullptr;
        const auto p=sparse_root_.find(id);return p==sparse_root_.end()?nullptr:p->second.get();
    }
    static std::size_t group_index(std::size_t i){return (i>>(cell_bits_+page_bits_))&(groups_-1);}
    static std::size_t page_index(std::size_t i){return (i>>cell_bits_)&(pages_-1);}
    Page* find_page(std::size_t i)const {
        auto* b=find_block(i>>block_bits_);if(!b)return nullptr;
        auto* g=b->groups[group_index(i)].get();return g?g->pages[page_index(i)].get():nullptr;
    }
    void clear_storage() {
        root_.clear();sparse_root_.clear();entries_=0;allocated_pages_=0;allocated_groups_=0;allocated_blocks_=0;
    }
    T& insert(std::size_t i) {
        const auto bi=i>>block_bits_,gi=group_index(i),pi=page_index(i),ci=i&(cells_-1);
        std::unique_ptr<Block> new_block;std::unique_ptr<Group> new_group;std::unique_ptr<Page> new_page;
        auto* b=find_block(bi);if(!b){new_block=std::make_unique<Block>();b=new_block.get();}
        auto* g=b->groups[gi].get();if(!g){new_group=std::make_unique<Group>();g=new_group.get();}
        auto* p=g->pages[pi].get();if(!p){new_page=std::make_unique<Page>();p=new_page.get();}
        if(p->present&(std::uint64_t(1)<<ci))return p->get(ci);
        const bool add_block=bool(new_block),add_group=bool(new_group),add_page=bool(new_page);
        T& result=p->create(ci,default_);
        // Commit lower ownership only into a local parent when that parent is new.
        // If root allocation/insertion throws, local RAII destroys the new cell.
        if(new_page){g->pages[pi]=std::move(new_page);++g->count;}
        if(new_group){b->groups[gi]=std::move(new_group);++b->count;}
        if(new_block) {
            if(direct_root()) {
                if(root_.empty())root_.resize(roots_for(domain_));
                root_[bi]=std::move(new_block);
            }else {
                const auto inserted=sparse_root_.try_emplace(bi,std::move(new_block));
                if(!inserted.second)throw std::logic_error("CTA paged root collision");
            }
        }
        ++entries_;peak_=std::max(peak_,entries_);
        allocated_pages_+=add_page;allocated_groups_+=add_group;allocated_blocks_+=add_block;
        return result;
    }
public:
    CtaSparseSlots()=default;
    explicit CtaSparseSlots(std::size_t n):domain_(n){}
    CtaSparseSlots(std::size_t n,const T& value):domain_(n),default_(value){}
    CtaSparseSlots(const CtaSparseSlots&)=delete;
    CtaSparseSlots&operator=(const CtaSparseSlots&)=delete;
    std::size_t size()const{return domain_;}
    std::size_t entries()const{return entries_;}
    std::size_t peak_entries()const{return peak_;}
    std::size_t entry_payload_bytes()const{return entries_*sizeof(T);}
    // Historical method name retained for callers; value now means live pages.
    // It is host storage telemetry, not a fabricated hash bucket count.
    std::size_t bucket_count()const{return allocated_pages_;}
    std::size_t allocated_groups()const{return allocated_groups_;}
    std::size_t allocated_directory_blocks()const{return allocated_blocks_;}
    std::size_t allocated_root_capacity()const{return root_.capacity();}
    bool uses_sparse_root_fallback()const{return !direct_root();}
    // Requested object storage including unused cell capacity; allocator overhead
    // and sparse-root hash nodes/buckets are excluded from this diagnostic.
    std::size_t allocated_object_bytes_excluding_hash_root()const {
        return root_.capacity()*sizeof(std::unique_ptr<Block>)+allocated_blocks_*sizeof(Block)+
            allocated_groups_*sizeof(Group)+allocated_pages_*sizeof(Page);
    }
    void assign(std::size_t n,const T& value){clear_storage();domain_=n;default_=value;}
    void resize(std::size_t n,const T& value) {
        if(n<domain_)throw std::logic_error("CTA sparse domain cannot shrink");
        if(entries_&&n!=domain_)throw std::logic_error("CTA sparse populated domain resize");
        if(n!=domain_)clear_storage();domain_=n;default_=value;
    }
    const T&operator[](std::size_t i)const {
        bounds(i);const auto* p=find_page(i);const auto ci=i&(cells_-1);
        return p&&(p->present&(std::uint64_t(1)<<ci))?p->get(ci):default_;
    }
    T&operator[](std::size_t i){
        bounds(i);auto* p=find_page(i);const auto ci=i&(cells_-1);
        // Keep the common mutable-read/update path small enough to inline;
        // ownership allocation and exception rollback remain in cold insert.
        if(p&&(p->present&(std::uint64_t(1)<<ci)))return p->get(ci);
        return insert(i);
    }
    void erase_slot(std::size_t i) {
        bounds(i);const auto bi=i>>block_bits_,gi=group_index(i),pi=page_index(i),ci=i&(cells_-1);
        auto* b=find_block(bi);if(!b)return;auto* g=b->groups[gi].get();if(!g)return;
        auto* p=g->pages[pi].get();if(!p||!(p->present&(std::uint64_t(1)<<ci)))return;
        --entries_;p->erase(ci);
        if(!p->present){g->pages[pi].reset();--g->count;--allocated_pages_;}
        if(!g->count){b->groups[gi].reset();--b->count;--allocated_groups_;}
        if(!b->count){if(direct_root())root_[bi].reset();else sparse_root_.erase(bi);--allocated_blocks_;}
    }
};

#else
template<class T> class CtaSparseSlots {
    std::size_t domain_ = 0, peak_ = 0;
    std::unordered_map<std::size_t,T> slots_;
    T default_{};
    void bounds(std::size_t i) const {
        if (i >= domain_) throw std::out_of_range("CTA sparse logical ID");
    }
public:
    CtaSparseSlots() = default;
    explicit CtaSparseSlots(std::size_t n):domain_(n) {}
    CtaSparseSlots(std::size_t n,const T& value):domain_(n),default_(value) {}
    CtaSparseSlots(const CtaSparseSlots&) = delete;
    CtaSparseSlots& operator=(const CtaSparseSlots&) = delete;
    std::size_t size() const { return domain_; }
    std::size_t entries() const { return slots_.size(); }
    std::size_t peak_entries() const { return peak_; }
    std::size_t entry_payload_bytes() const { return slots_.size()*sizeof(T); }
    std::size_t bucket_count() const { return slots_.bucket_count(); }
    void assign(std::size_t n,const T& value) {
        slots_.clear(); domain_=n; default_=value;
    }
    void resize(std::size_t n,const T& value) {
        if (n<domain_) throw std::logic_error("CTA sparse domain cannot shrink");
        if (!slots_.empty() && n!=domain_)
            throw std::logic_error("CTA sparse populated domain resize");
        domain_=n; default_=value;
    }
    const T& operator[](std::size_t i) const {
        bounds(i); auto p=slots_.find(i); return p==slots_.end()?default_:p->second;
    }
    T& operator[](std::size_t i) {
        bounds(i); auto p=slots_.find(i);
        if(p==slots_.end()) {
            if constexpr(std::is_copy_constructible_v<T>)
                p=slots_.try_emplace(i,default_).first;
            else p=slots_.try_emplace(i).first;
            peak_=std::max(peak_,slots_.size());
        }
        return p->second;
    }
    void erase_slot(std::size_t i) { bounds(i); slots_.erase(i); }
};

#endif

// Match vector<bool>'s proxy contract: testing the scheduler's last retired warp
// must not allocate a tombstone slot on every future dispatch. Writes still own
// independent slots; reads of absent flags return the declared false default.
template<> class CtaSparseSlots<bool> {
    CtaSparseSlots<unsigned char> slots_;
    bool get(std::size_t i) const { return slots_[i]!=0; }
public:
    class Reference {
        CtaSparseSlots* owner_;std::size_t index_;
    public:
        Reference(CtaSparseSlots* owner,std::size_t index):owner_(owner),index_(index) {}
        operator bool() const { return owner_->get(index_); }
        Reference& operator=(bool value) { owner_->slots_[index_]=value;return *this; }
        Reference& operator=(const Reference& value) {return *this=static_cast<bool>(value);}
    };
    CtaSparseSlots()=default;
    explicit CtaSparseSlots(std::size_t n):slots_(n) {}
    CtaSparseSlots(std::size_t n,bool value):slots_(n,value) {}
    std::size_t size() const{return slots_.size();}
    std::size_t entries() const{return slots_.entries();}
    std::size_t peak_entries() const{return slots_.peak_entries();}
    std::size_t entry_payload_bytes() const{return slots_.entry_payload_bytes();}
    std::size_t bucket_count() const{return slots_.bucket_count();}
    void assign(std::size_t n,bool value){slots_.assign(n,value);}
    void resize(std::size_t n,bool value){slots_.resize(n,value);}
    bool operator[](std::size_t i) const{return get(i);}
    Reference operator[](std::size_t i){(void)get(i);return Reference(this,i);}
    void erase_slot(std::size_t i){slots_.erase_slot(i);}
};
} // namespace GTSim
