#pragma once
// Single-threaded host storage only. DAGNode pointers remain non-owning.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace GTSim {
class DAGNode;

class LazyReadyQueue {
public:
    using value_type=DAGNode*;
    using size_type=std::size_t;
    using difference_type=std::ptrdiff_t;
    using Storage=std::deque<value_type>;
    struct Statistics {
        std::uint64_t live_deques;
        std::uint64_t peak_live_deques;
        std::uint64_t total_deque_constructions;
        std::uint64_t destroyed_deques;
    };

    // Minimal iterator contract used by the frozen scheduler: range traversal,
    // begin()+index and immediate erase. Not a general std::deque substitute.
    // Queue mutation/move/destruction invalidates outstanding iterators. A
    // same-index stale iterator cannot be detected without per-queue epochs;
    // callers must not reuse it. Current scheduler keeps none across mutations.
    template<bool IsConst> class BasicIterator {
        using Owner=typename std::conditional<IsConst,const LazyReadyQueue,LazyReadyQueue>::type;
        Owner* owner_=nullptr;
        size_type index_=0;
        BasicIterator(Owner* owner,size_type index):owner_(owner),index_(index){}
        friend class LazyReadyQueue;
        template<bool> friend class BasicIterator;
    public:
        using iterator_category=std::forward_iterator_tag;
        using value_type=DAGNode*;
        using difference_type=std::ptrdiff_t;
        using reference=typename std::conditional<IsConst,value_type const&,value_type&>::type;
        using pointer=typename std::conditional<IsConst,value_type const*,value_type*>::type;
        BasicIterator()=default;
        template<bool Other,typename std::enable_if<IsConst&&!Other,int>::type=0>
        BasicIterator(const BasicIterator<Other>& other):owner_(other.owner_),index_(other.index_){}

        reference operator*()const {
            if(!owner_)throw std::invalid_argument("default lazy queue iterator");
            return (*owner_)[index_];
        }
        pointer operator->()const{return std::addressof(operator*());}
        BasicIterator operator+(difference_type distance)const {
            if(!owner_)throw std::invalid_argument("default lazy queue iterator arithmetic");
            const size_type size=owner_->size();
            if(index_>size)throw std::out_of_range("lazy queue iterator outside current extent");
            size_type next=index_;
            if(distance>=0) {
                const auto amount=static_cast<size_type>(distance);
                if(amount>size-index_)throw std::out_of_range("lazy queue iterator after end");
                next+=amount;
            } else {
                // Avoid signed overflow for PTRDIFF_MIN.
                const auto amount=static_cast<size_type>(-(distance+1))+1;
                if(amount>index_)throw std::out_of_range("lazy queue iterator before begin");
                next-=amount;
            }
            return BasicIterator(owner_,next);
        }
        BasicIterator& operator+=(difference_type distance){*this=*this+distance;return *this;}
        BasicIterator& operator++(){return *this+=1;}
        BasicIterator operator++(int){auto old=*this;++*this;return old;}
        template<bool Other> bool operator==(const BasicIterator<Other>& other)const noexcept {
            return owner_==other.owner_&&index_==other.index_;
        }
        template<bool Other> bool operator!=(const BasicIterator<Other>& other)const noexcept{return !(*this==other);}
        friend BasicIterator operator+(difference_type distance,const BasicIterator& iterator){return iterator+distance;}
    };
    using iterator=BasicIterator<false>;
    using const_iterator=BasicIterator<true>;

    LazyReadyQueue()noexcept=default;
    LazyReadyQueue(const LazyReadyQueue&)=delete;
    LazyReadyQueue& operator=(const LazyReadyQueue&)=delete;
    LazyReadyQueue(LazyReadyQueue&& other)noexcept:queue_(std::move(other.queue_)){}
    LazyReadyQueue& operator=(LazyReadyQueue&& other)noexcept {
        if(this!=&other){destroy_storage();queue_=std::move(other.queue_);}
        return *this;
    }
    ~LazyReadyQueue(){destroy_storage();}

    bool empty()const noexcept{return !queue_||queue_->empty();}
    size_type size()const noexcept{return queue_?queue_->size():0;}
    value_type& front(){return (*this)[0];}
    value_type const& front()const{return (*this)[0];}
    value_type& operator[](size_type index) {
        if(index>=size())throw std::out_of_range("lazy queue element outside extent");
        return (*queue_)[index];
    }
    value_type const& operator[](size_type index)const {
        if(index>=size())throw std::out_of_range("lazy queue element outside extent");
        return (*queue_)[index];
    }
    void push_back(value_type value) {
        if(!queue_) {
            // Publish/count only after successful deque construction. A later
            // push failure may leave this empty deque, as does ordinary clear.
            queue_=std::make_unique<Storage>();
            auto& s=counters();++s.live_deques;++s.total_deque_constructions;
            if(s.live_deques>s.peak_live_deques)s.peak_live_deques=s.live_deques;
        }
        queue_->push_back(value);
    }
    void pop_front() {
        if(empty())throw std::out_of_range("pop of empty lazy queue");
        queue_->pop_front(); // Retain storage until owning Scheduler destruction.
    }
    void clear()noexcept{if(queue_)queue_->clear();}
    iterator begin()noexcept{return iterator(this,0);}
    iterator end()noexcept{return iterator(this,size());}
    const_iterator begin()const noexcept{return const_iterator(this,0);}
    const_iterator end()const noexcept{return const_iterator(this,size());}
    const_iterator cbegin()const noexcept{return begin();}
    const_iterator cend()const noexcept{return end();}
    iterator erase(iterator position){return erase_index(position.owner_,position.index_);}
    iterator erase(const_iterator position){return erase_index(position.owner_,position.index_);}

    // Deque object lifetimes, NOT allocator calls/requested bytes/RSS. Non-atomic
    // by contract: the admitted simulator and these diagnostics are single-threaded.
    static Statistics statistics()noexcept{return counters();}
    static void reset_statistics() {
        auto& s=counters();
        if(s.live_deques!=0)throw std::logic_error("cannot reset live lazy deque statistics");
        s=Statistics{0,0,0,0};
    }
private:
    iterator erase_index(const LazyReadyQueue* owner,size_type index) {
        if(owner!=this)throw std::invalid_argument("foreign/default lazy queue erase iterator");
        if(index>=size())throw std::out_of_range("erase outside lazy queue extent");
        queue_->erase(queue_->begin()+static_cast<difference_type>(index));
        return iterator(this,index);
    }
    void destroy_storage()noexcept {
        if(queue_) {
            queue_.reset();
            auto& s=counters();--s.live_deques;++s.destroyed_deques;
        }
    }
    static Statistics& counters()noexcept {
        static Statistics value{0,0,0,0};return value;
    }
    std::unique_ptr<Storage> queue_; // The sole per-global-warp field.
};
} // namespace GTSim
