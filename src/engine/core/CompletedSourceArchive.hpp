// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace openswmm::detail {
// Internal, same-platform, versioned source-boundary archive. Explicit fields
// only: no object padding, pointers, streams or raw solver objects on disk.
class CompletedSourceArchive {
public:
    CompletedSourceArchive() = default;
    explicit CompletedSourceArchive(std::string bytes) : bytes_(std::move(bytes)), reading_(true) {}
    bool reading() const { return reading_; }
    const std::string& bytes() const { return bytes_; }
    void finish() const { if(reading_ && offset_!=bytes_.size()) fail(); }
    template<class... T> void operator()(T&... values) { (field(values),...); }
    template<class... T> void fixed(T&... values) { (fixedOne(values),...); }
    template<class T,class F> void sequence(std::vector<T>& value,F visit,bool fixed_size=false) {
        uint64_t size=value.size();field(size);
        allocate(size,sizeof(T));
        if(reading_) {
            if(size>bytes_.size()-offset_)fail();
            if(fixed_size && size!=value.size())throw std::runtime_error("Checkpoint vector shape differs: saved "+std::to_string(size)+", model "+std::to_string(value.size())+", offset "+std::to_string(offset_)+".");
            value.resize(static_cast<std::size_t>(size));
        }
        for(auto& element:value)visit(element);
    }
private:
    std::string bytes_;
    std::size_t offset_=0;
    bool reading_=false;
    uint64_t allocation_remaining_=64*1024*1024;
    void allocate(uint64_t count,uint64_t width) {
        if(count>allocation_remaining_/width)fail();allocation_remaining_-=count*width;
    }
    [[noreturn]] static void fail() { throw std::runtime_error("Invalid completed-source checkpoint record."); }
    template<class T> void field(T& value) requires (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
        if(reading_) {
            if(sizeof(T)>bytes_.size()-offset_)fail();
            // Invalid bool representations must not enter a C++ object.
            if constexpr(std::is_same_v<T,bool>) {
                const auto v=static_cast<unsigned char>(bytes_[offset_]);if(v>1)fail();value=v!=0;
            } else std::memcpy(&value,bytes_.data()+offset_,sizeof(T));
            offset_+=sizeof(T);
        } else bytes_.append(reinterpret_cast<const char*>(&value),sizeof(T));
    }
    void field(std::string& value) {
        uint64_t size=value.size();field(size);allocate(size,1);
        if(reading_) {
            if(size>bytes_.size()-offset_)fail();
            value.assign(bytes_.data()+offset_,static_cast<std::size_t>(size));offset_+=size;
        } else bytes_.append(value);
    }
    template<class T> void fixedOne(std::vector<T>& value) {
        sequence(value,[&](auto& element){field(element);},true);
    }
    void fixedOne(std::vector<bool>& value) {
        uint64_t size=value.size();field(size);
        if(reading_&&(size!=value.size()||size>bytes_.size()-offset_))fail();
        for(std::size_t i=0;i<value.size();++i){bool element=value[i];field(element);if(reading_)value[i]=element;}
    }
    template<class T> void field(std::vector<T>& value) {
        sequence(value,[&](auto& element){field(element);});
    }
    void field(std::vector<bool>& value) {
        uint64_t size=value.size();field(size);
        allocate(size,1);
        if(reading_) { if(size>bytes_.size()-offset_)fail();value.resize(size); }
        for(std::size_t i=0;i<value.size();++i){bool element=value[i];field(element);if(reading_)value[i]=element;}
    }
    template<class T,std::size_t N> void field(std::array<T,N>& value) { for(auto& element:value)field(element); }
    template<class A,class B> void field(std::pair<A,B>& value) { field(value.first);field(value.second); }
};
}
