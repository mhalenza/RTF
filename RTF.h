// Copyright (c) 2024 Matt M Halenza
// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <concepts>
#include <format>
#include <memory>
#include <thread>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include <assert.h>
#include <stdint.h>
#ifdef RTF_INTEROP_RMF
#include <RMF/RMF.h>
#ifndef RMF_EXPLICIT_ADDRESSTYPE_CONVERSION_OPERATOR
#warning "RTF_INTEROP_RMF enabled but RMF_EXPLICIT_ADDRESSTYPE_CONVERSION_OPERATOR is not defined.  Strongly consider defining it for safety!
#endif
#endif

namespace RTF {

template <typename T>
concept ValidAddressOrDataType = std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>;

template <ValidAddressOrDataType AddressType_, ValidAddressOrDataType DataType_>
struct IRegisterTarget
{
protected:
    IRegisterTarget(std::string_view name) : name(name) {}
public:
    using AddressType = AddressType_;
    using DataType = DataType_;
    virtual ~IRegisterTarget() = default;

    virtual std::string_view getName() const { return this->name; }
    virtual std::string_view getDomain() const { return "IRegisterTarget"; }

    virtual void write(AddressType addr, DataType data) = 0;
    [[nodiscard]] virtual DataType read(AddressType addr) = 0;

    virtual void readModifyWrite(AddressType addr, DataType new_data, DataType mask)
    {
        DataType v = this->read(addr);
        v &= ~mask;
        v |= new_data & mask;
        this->write(addr, v);
    }

    virtual void seqWrite(AddressType start_addr, std::span<DataType const> data, size_t increment = sizeof(DataType))
    {
        for (size_t i = 0 ; i < data.size() ; i++) {
            this->write(AddressType(start_addr + (increment * i)), data[i]);
        }
    }
    virtual void seqRead(AddressType start_addr, std::span<DataType> out_data, size_t increment = sizeof(DataType))
    {
        for (size_t i = 0 ; i < out_data.size() ; i++) {
            out_data[i] = this->read(AddressType(start_addr + (increment * i)));
        }
    }

    virtual void fifoWrite(AddressType fifo_addr, std::span<DataType const> data)
    {
        for (auto const d : data) {
            this->write(fifo_addr, d);
        }
    }
    virtual void fifoRead(AddressType fifo_addr, std::span<DataType> out_data)
    {
        for (auto& d : out_data) {
            d = this->read(fifo_addr);
        }
    }

    virtual void compWrite(std::span<std::pair<AddressType, DataType> const> addr_data)
    {
        for (auto const ad : addr_data) {
            this->write(ad.first, ad.second);
        }
    }
    virtual void compRead(std::span<AddressType const> const addresses, std::span<DataType> out_data)
    {
        assert(addresses.size() == out_data.size());
        for (size_t i = 0 ; i < addresses.size() ; i++) {
            out_data[i] = this->read(addresses[i]);
        }
    }
private:
    std::string name;
};

template <typename PollerType>
concept CPoller = requires(PollerType const &p)
{
    { p([]() -> bool { return true; }) } -> std::convertible_to<bool>;
};

class BasicPoller
{
public:
    BasicPoller(std::chrono::microseconds initial_delay, std::chrono::microseconds wait_delay, std::chrono::microseconds timeout)
        : initial_delay(initial_delay)
        , wait_delay(wait_delay)
        , timeout(timeout)
    {}

    template <typename CheckFunctorType>
    bool operator()(CheckFunctorType fn) const
    {
        std::this_thread::sleep_for(this->initial_delay);
        auto const start_timestamp = std::chrono::steady_clock::now();
        do {
            if (fn())
                return true;
            std::this_thread::sleep_for(this->wait_delay);
        } while (std::chrono::steady_clock::now() < start_timestamp + this->timeout);
        return false;
    }

private:
    std::chrono::microseconds initial_delay;
    std::chrono::microseconds wait_delay;
    std::chrono::microseconds timeout;
};
static_assert(CPoller<BasicPoller>);

extern BasicPoller const default_poller;
#ifdef RTF_IMPLEMENTATION
#ifndef RTF_DEFAULT_POLLER_INITIAL_DELAY
#define RTF_DEFAULT_POLLER_INITIAL_DELAY std::chrono::seconds(0)
#endif
#ifndef RTF_DEFAULT_POLLER_RECHECK_DELAY
#define RTF_DEFAULT_POLLER_RECHECK_DELAY std::chrono::microseconds(500)
#endif
#ifndef RTF_DEFAULT_POLLER_TIMEOUT
#define RTF_DEFAULT_POLLER_TIMEOUT std::chrono::seconds(3)
#endif
BasicPoller const default_poller = {
    RTF_DEFAULT_POLLER_INITIAL_DELAY,
    RTF_DEFAULT_POLLER_RECHECK_DELAY,
    RTF_DEFAULT_POLLER_TIMEOUT
};
#endif

namespace Operations {

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
using AddressRegType = std::variant<
    AddressType
    #ifdef RTF_INTEROP_RMF
    , std::reference_wrapper<::RMF::Register<AddressType, DataType> const>
    #endif
>;
template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
using AddressRegFieldType = std::variant<
    AddressType
    #ifdef RTF_INTEROP_RMF
    , std::reference_wrapper<::RMF::Register<AddressType, DataType> const>
    , std::reference_wrapper<::RMF::Field<AddressType, DataType> const>
    #endif
>;

/* Note for the Operations structs while RTF_INTEROP_RMF:
*   When the struct contains a `AddressRegFieldType<AddressType, DataType> address` field,
*   A corresponding `data` field always represents the full register value.
*   This is obviously correct for when the variant holds an AddressType or an RMF::Register<AT,DT>.
*   But it's not intuitive for the case when it holds an RMF::Field<AD,DT>.
*   It would have made sense for the `data` field to hold just the field data,
*   but the interposer almost certainly wants to log the whole register value in addition to the specific field value.
*   I didn't want to *also* make the `data` field a variant because that would have been hell.
*   Instead, `data` always holds the full register value and the interposer can easily use
*   the RMF::Field<AT,DR> to extract the field value for logging.
*/

struct Seq {
    std::string_view msg;
};
struct Step {
    std::string_view msg;
};

struct Null {
    std::string_view msg;
};
struct Delay {
    std::chrono::microseconds delay;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct Write {
    AddressRegFieldType<AddressType, DataType> address;
    DataType data;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct Read {
    AddressRegFieldType<AddressType, DataType> address;
    DataType const& out_data;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct ReadModifyWrite {
    AddressRegFieldType<AddressType, DataType> address;
    DataType new_data;
    DataType mask;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct SeqWrite {
    AddressRegType<AddressType, DataType> start_address;
    std::span<DataType const> data;
    size_t increment;
    std::string_view msg;
};
template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct SeqRead {
    AddressRegType<AddressType, DataType> start_address;
    std::span<DataType const> out_data;
    size_t increment;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct FifoWrite {
    AddressRegType<AddressType, DataType> fifo_address;
    std::span<DataType const> data;
    std::string_view msg;
};
template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct FifoRead {
    AddressRegType<AddressType, DataType> fifo_address;
    std::span<DataType const> out_data;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct CompWrite {
    //TODO: AddressRegType<AddressType, DataType> in here somehow
    std::span<std::pair<AddressType, DataType> const> address_data;
    std::string_view msg;
};
template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct CompRead {
    //TODO: AddressRegType<AddressType, DataType> in here somehow
    std::span<AddressType const> addresses;
    std::span<DataType const> out_data;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct WriteVerify {
    AddressRegFieldType<AddressType, DataType> address;
    DataType data;
    DataType mask;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct ReadVerify {
    AddressRegFieldType<AddressType, DataType> address;
    DataType expected;
    DataType mask;
    std::string_view msg;
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct PollRead {
    AddressRegFieldType<AddressType, DataType> address;
    DataType expected;
    DataType mask;
    std::string_view msg;
};

}

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
class IFluentInterposer
{
public:
    virtual ~IFluentInterposer() = default;

    virtual void seq(std::string_view target_domain, std::string_view target_instance, Operations::Seq const& seq) = 0;
    virtual void step(std::string_view target_domain, std::string_view target_instance, Operations::Step const& step) = 0;

    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::Null const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::Null const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::Delay const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::Delay const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::Write<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::Write<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::Read<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::Read<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::ReadModifyWrite<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::ReadModifyWrite<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::SeqWrite<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::SeqWrite<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::SeqRead<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::SeqRead<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::FifoWrite<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::FifoWrite<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::FifoRead<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::FifoRead<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::CompWrite<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::CompWrite<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::CompRead<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::CompRead<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::WriteVerify<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::WriteVerify<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::ReadVerify<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::ReadVerify<AddressType, DataType> const& op) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, Operations::PollRead<AddressType, DataType> const& op) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, Operations::PollRead<AddressType, DataType> const& op) = 0;

    virtual void error(std::string_view target_domain, std::string_view target_instance, std::exception const& ex) = 0;

    static void setDefault(std::unique_ptr<IFluentInterposer> new_default_interposer)
    {
        default_interposer = std::move(new_default_interposer);
    }
    static IFluentInterposer* getDefault()
    {
        return default_interposer.get();
    }

private:
    static std::unique_ptr<IFluentInterposer> default_interposer;
};

#ifdef RTF_IMPLEMENTATION
std::unique_ptr<IFluentInterposer<uint8_t, uint8_t>> IFluentInterposer<uint8_t, uint8_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint8_t, uint16_t>> IFluentInterposer<uint8_t, uint16_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint8_t, uint32_t>> IFluentInterposer<uint8_t, uint32_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint8_t, uint64_t>> IFluentInterposer<uint8_t, uint64_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint16_t, uint8_t>> IFluentInterposer<uint16_t, uint8_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint16_t, uint16_t>> IFluentInterposer<uint16_t, uint16_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint16_t, uint32_t>> IFluentInterposer<uint16_t, uint32_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint16_t, uint64_t>> IFluentInterposer<uint16_t, uint64_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint32_t, uint8_t>> IFluentInterposer<uint32_t, uint8_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint32_t, uint16_t>> IFluentInterposer<uint32_t, uint16_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint32_t, uint32_t>> IFluentInterposer<uint32_t, uint32_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint32_t, uint64_t>> IFluentInterposer<uint32_t, uint64_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint64_t, uint8_t>> IFluentInterposer<uint64_t, uint8_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint64_t, uint16_t>> IFluentInterposer<uint64_t, uint16_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint64_t, uint32_t>> IFluentInterposer<uint64_t, uint32_t>::default_interposer = nullptr;
std::unique_ptr<IFluentInterposer<uint64_t, uint64_t>> IFluentInterposer<uint64_t, uint64_t>::default_interposer = nullptr;
#endif

class IConsolidatedFluentInterposer
{
public:
    virtual ~IConsolidatedFluentInterposer() = default;
    virtual void seq(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void step(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void op(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void extra(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void end(std::string_view target_domain, std::string_view target_instance) = 0;
    virtual void error(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;

    static void setDefault(std::unique_ptr<IConsolidatedFluentInterposer> new_default_interposer)
    {
        default_interposer = std::move(new_default_interposer);
    }
    static IConsolidatedFluentInterposer* getDefault()
    {
        return default_interposer.get();
    }

private:
    static std::unique_ptr<IConsolidatedFluentInterposer> default_interposer;
};
#ifdef RTF_IMPLEMENTATION
std::unique_ptr<IConsolidatedFluentInterposer> IConsolidatedFluentInterposer::default_interposer = nullptr;
#endif

enum class OperationFormattingVerbosity
{
    eMinimal,
    eCompact,
    eFull,
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
class BasicFluentInterposerAdapter : public IFluentInterposer<AddressType, DataType>
{
private:
    template <typename... Args>
    void op(std::string_view target_domain, std::string_view target_instance, std::format_string<Args...> fmt, Args... args)
    {
        if (this->consolidated) {
            this->consolidated->op(target_domain, target_instance, std::vformat(fmt.get(), std::make_format_args(args...)));
        }
    }
    template <typename... Args>
    void end(std::string_view target_domain, std::string_view target_instance, std::format_string<Args...> fmt, Args... args)
    {
        if (this->consolidated) {
            this->consolidated->end(target_domain, target_instance, std::vformat(fmt.get(), std::make_format_args(args...)));
        }
    }
    void end(std::string_view target_domain, std::string_view target_instance)
    {
        if (this->consolidated) {
            this->consolidated->end(target_domain, target_instance);
        }
    }
    void extra(std::string_view target_domain, std::string_view target_instance, std::span<DataType const> data)
    {
        if (this->consolidated) {
            if (data.size() <= this->array_size_limit) {
                if (this->verbosity == OperationFormattingVerbosity::eCompact) {
                    std::string msg;
                    for (auto d : data) {
                        msg += std::format("0x{:0{}x}, ", d, sizeof(DataType) * 2);
                    }
                    this->consolidated->extra(target_domain, target_instance, msg);
                }
                else if (this->verbosity == OperationFormattingVerbosity::eFull) {
                    for (auto d : data) {
                        this->consolidated->extra(target_domain, target_instance, std::format("0x{:0{}x}", d, sizeof(DataType) * 2));
                    }
                }
            }
        }
    }
    void extra(std::string_view target_domain, std::string_view target_instance, std::span<AddressType const> addresses)
        requires (!std::is_same_v<AddressType, DataType>)
    {
        if (this->consolidated) {
            if (addresses.size() <= this->array_size_limit) {
                if (this->verbosity == OperationFormattingVerbosity::eCompact) {
                    std::string msg;
                    for (auto a : addresses) {
                        msg += std::format("0x{:0{}x}, ", a, sizeof(AddressType) * 2);
                    }
                    this->consolidated->extra(target_domain, target_instance, msg);
                }
                else if (this->verbosity == OperationFormattingVerbosity::eFull) {
                    for (auto a : addresses) {
                        this->consolidated->extra(target_domain, target_instance, std::format("0x{:0{}x}", a, sizeof(AddressType) * 2));
                    }
                }
            }
        }
    }
    void extra(std::string_view target_domain, std::string_view target_instance, std::span<std::pair<AddressType, DataType> const> addr_data)
    {
        if (this->consolidated) {
            if (addr_data.size() <= this->array_size_limit) {
                if (this->verbosity == OperationFormattingVerbosity::eCompact) {
                    std::string msg;
                    for (auto ad : addr_data) {
                        msg += std::format("0x{:0{}x}=0x{:0{}x}, ", ad.first, sizeof(AddressType) * 2, ad.second, sizeof(DataType) * 2);
                    }
                    this->consolidated->extra(target_domain, target_instance, msg);
                }
                else if (this->verbosity == OperationFormattingVerbosity::eFull) {
                    for (auto ad : addr_data) {
                        this->consolidated->extra(target_domain, target_instance, std::format("0x{:0{}x}=0x{:0{}x}, ", ad.first, sizeof(AddressType) * 2, ad.second, sizeof(DataType) * 2));
                    }
                }
            }
        }
    }
public:
    BasicFluentInterposerAdapter(IConsolidatedFluentInterposer* consolidated, OperationFormattingVerbosity verb = OperationFormattingVerbosity::eFull, size_t array_size_limit = 4096)
        : consolidated(consolidated)
        , verbosity(verb)
        , array_size_limit(array_size_limit)
    {}
    explicit BasicFluentInterposerAdapter(OperationFormattingVerbosity verb = OperationFormattingVerbosity::eFull, size_t array_size_limit = 4096)
        : consolidated(IConsolidatedFluentInterposer::getDefault())
        , verbosity(verb)
        , array_size_limit(array_size_limit)
    {}

    virtual void seq(std::string_view td, std::string_view ti, Operations::Seq const& seq) override
    {
        if (this->consolidated)
            this->consolidated->seq(td, ti, seq.msg);
    }
    virtual void step(std::string_view td, std::string_view ti, Operations::Step const& step) override
    {
        if (this->consolidated)
            this->consolidated->step(td, ti, step.msg);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::Null const& op) override
    {
        this->op(td, ti, "Null(): {}",
                 op.msg);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::Null const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::Delay const& op) override
    {
        this->op(td, ti, "Delay({}): {}",
                 op.delay,
                 op.msg);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::Delay const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::Write<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "Write(0x{:0{}x}, 0x{:0{}x}): {}",
                         addr, sizeof(AddressType) * 2,
                         op.data, sizeof(DataType) * 2,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->op(td, ti, "Write(0x{:0{}x} '{}', 0x{:0{}x}): {}",
                         reg.address(), sizeof(AddressType) * 2,
                         reg.fullName(),
                         op.data, sizeof(DataType) * 2,
                         op.msg);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->op(td, ti, "Write(0x{:0{}x} '{}', 0x{:0{}x} (0x{:0{}x} & 0x{:0{}x}): {}",
                         field.address(), sizeof(AddressType) * 2,
                         field.fullName(),
                         field.extract(op.data), (field.size() + 3) / 4,
                         op.data, sizeof(DataType) * 2,
                         field.regMask(), sizeof(DataType) * 2,
                         op.msg);
            }
            #endif
        }, op.address);

    }
    virtual void end(std::string_view td, std::string_view ti, Operations::Write<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::Read<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "Read(0x{:0{}x}): {}",
                         addr, sizeof(AddressType) * 2,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->op(td, ti, "Read(0x{:0{}x} '{}'): {}",
                         reg.address(), sizeof(AddressType) * 2,
                         reg.fullName(),
                         op.msg);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->op(td, ti, "Read(0x{:0{}x} '{}'): {}",
                         field.address(), sizeof(AddressType) * 2,
                         field.fullName(),
                         op.msg);
            }
            #endif
        }, op.address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::Read<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->end(td, ti, "ReadResp: 0x{:0{}x}",
                          op.out_data, sizeof(DataType) * 2);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->end(td, ti, "ReadResp: 0x{:0{}x}",
                          op.out_data, sizeof(DataType) * 2);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->end(td, ti, "ReadResp: 0x{:0{}x} (0x{:0{}x} & 0x{:0{}x})",
                          field.extract(op.out_data), (field.size() + 3) / 4,
                          op.out_data, sizeof(DataType) * 2,
                          field.regMask(), sizeof(DataType) * 2);
            }
            #endif
        }, op.address);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::ReadModifyWrite<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "ReadModifyWrite(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}",
                         addr, sizeof(AddressType) * 2,
                         op.new_data & op.mask, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->op(td, ti, "ReadModifyWrite(0x{:0{}x} '{}', 0x{:0{}x}, 0x{:0{}x}): {}",
                         reg.address(), sizeof(AddressType) * 2,
                         reg.fullName(),
                         op.new_data & op.mask, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->op(td, ti, "ReadModifyWrite(0x{:0{}x} '{}', 0x{:0{}x} (0x{:0{}x} & 0x{:0{}x})): {}",
                         field.address(), sizeof(AddressType) * 2,
                         field.fullName(),
                         field.extract(op.new_data), (field.size() + 3) / 4,
                         op.new_data, sizeof(DataType) * 2,
                         field.regMask(), sizeof(DataType) * 2,
                         op.msg);
            }
            #endif
        }, op.address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::ReadModifyWrite<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::SeqWrite<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& start_addr) {
            using T = std::decay_t<decltype(start_addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "SeqWrite(0x{:0{}x}, {}.., {}): {}",
                         start_addr, sizeof(AddressType) * 2,
                         op.data.size(),
                         op.increment,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& start_reg = start_addr.get();
                this->op(td, ti, "SeqWrite(0x{:0{}x} '{}', {}.., {}): {}",
                         start_reg.address(), sizeof(AddressType) * 2,
                         start_reg.fullName(),
                         op.data.size(),
                         op.increment,
                         op.msg);
            }
            #endif
        }, op.start_address);
        this->extra(td, ti, op.data);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::SeqWrite<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::SeqRead<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& start_addr) {
            using T = std::decay_t<decltype(start_addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "SeqRead(0x{:0{}x}, {}.., {}): {}",
                         start_addr, sizeof(AddressType) * 2,
                         op.out_data.size(),
                         op.increment,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& start_reg = start_addr.get();
                this->op(td, ti, "SeqRead(0x{:0{}x} '{}', {}.., {}): {}",
                         start_reg.address(), sizeof(AddressType) * 2,
                         start_reg.fullName(),
                         op.out_data.size(),
                         op.increment,
                         op.msg);
            }
            #endif
        }, op.start_address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::SeqRead<AddressType, DataType> const& op) override
    {
        this->extra(td, ti, op.out_data);
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::FifoWrite<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& fifo_addr) {
            using T = std::decay_t<decltype(fifo_addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "FifoWrite(0x{:0{}x}, {}..): {}",
                         fifo_addr, sizeof(AddressType) * 2,
                         op.data.size(),
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& fifo_reg = fifo_addr.get();
                this->op(td, ti, "FifoWrite(0x{:0{}x} '{}', {}..): {}",
                         fifo_reg.address(), sizeof(AddressType) * 2,
                         fifo_reg.fullName(),
                         op.data.size(),
                         op.msg);
            }
            #endif
        }, op.fifo_address);
        this->extra(td, ti, op.data);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::FifoWrite<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::FifoRead<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& fifo_addr) {
            using T = std::decay_t<decltype(fifo_addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "FifoRead(0x{:0{}x}, {}): {}",
                         fifo_addr, sizeof(AddressType) * 2,
                         op.out_data.size(),
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& fifo_reg = fifo_addr.get();
                this->op(td, ti, "FifoRead(0x{:0{}x} '{}', {}): {}",
                         fifo_reg.address(), sizeof(AddressType) * 2,
                         fifo_reg.fullName(),
                         op.out_data.size(),
                         op.msg);
            }
            #endif
        }, op.fifo_address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::FifoRead<AddressType, DataType> const& op) override
    {
        this->extra(td, ti, op.out_data);
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::CompWrite<AddressType, DataType> const& op) override
    {
        this->op(td, ti, "CompWrite({}..): {}",
                 op.address_data.size(),
                 op.msg);
        this->extra(td, ti, op.address_data);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::CompWrite<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::CompRead<AddressType, DataType> const& op) override
    {
        this->op(td, ti, "CompRead({}.., {}..): {}",
                 op.addresses.size(),
                 op.out_data.size(),
                 op.msg);
        this->extra(td, ti, op.addresses);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::CompRead<AddressType, DataType> const& op) override
    {
        this->extra(td, ti, op.out_data);
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::WriteVerify<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "WriteVerify(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}",
                         addr, sizeof(AddressType) * 2,
                         op.data, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->op(td, ti, "WriteVerify(0x{:0{}x} '{}, 0x{:0{}x}, 0x{:0{}x}): {}",
                         reg.address(), sizeof(AddressType) * 2,
                         reg.fullName(),
                         op.data, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->op(td, ti, "WriteVerify(0x{:0{}x} '{}, 0x{:0{}x} (0x{:0{}x} & 0x{:0{}x})): {}",
                         field.address(), sizeof(AddressType) * 2,
                         field.fullName(),
                         field.extract(op.data), (field.size() + 3) / 4,
                         op.data, sizeof(DataType) * 2,
                         field.regMask(), sizeof(DataType) * 2,
                         op.msg);
            }
            #endif
        }, op.address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::WriteVerify<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::ReadVerify<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "ReadVerify(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}",
                         addr, sizeof(AddressType) * 2,
                         op.expected, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->op(td, ti, "ReadVerify(0x{:0{}x} '{}', 0x{:0{}x}, 0x{:0{}x}): {}",
                         reg.address(), sizeof(AddressType) * 2,
                         reg.fullName(),
                         op.expected, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->op(td, ti, "ReadVerify(0x{:0{}x} '{}, 0x{:0{}x} (0x{:0{}x} & 0x{:0{}x})): {}",
                         field.address(), sizeof(AddressType) * 2,
                         field.fullName(),
                         field.extract(op.expected), (field.size() + 3) / 4,
                         op.expected, sizeof(DataType) * 2,
                         field.regMask(), sizeof(DataType) * 2,
                         op.msg);
            }
            #endif
        }, op.address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::ReadVerify<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void op(std::string_view td, std::string_view ti, Operations::PollRead<AddressType, DataType> const& op) override
    {
        std::visit([&](auto&& addr) {
            using T = std::decay_t<decltype(addr)>;
            if constexpr (std::is_same_v<T, AddressType>) {
                this->op(td, ti, "PollRead(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}",
                         addr, sizeof(AddressType) * 2,
                         op.expected, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            #ifdef RTF_INTEROP_RMF
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Register<AddressType, DataType> const>>) {
                auto const& reg = addr.get();
                this->op(td, ti, "PollRead(0x{:0{}x} '{}', 0x{:0{}x}, 0x{:0{}x}): {}",
                         reg.address(), sizeof(AddressType) * 2,
                         reg.fullName(),
                         op.expected, sizeof(DataType) * 2,
                         op.mask, sizeof(DataType) * 2,
                         op.msg);
            }
            else if constexpr (std::is_same_v<T, std::reference_wrapper<::RMF::Field<AddressType, DataType> const>>) {
                auto const& field = addr.get();
                this->op(td, ti, "PollRead(0x{:0{}x} '{}', 0x{:0{}x} (0x{:0{}x} & 0x{:0{}x})): {}",
                         field.address(), sizeof(AddressType) * 2,
                         field.fullName(),
                         field.extract(op.expected), (field.size() + 3) / 4,
                         op.expected, sizeof(DataType) * 2,
                         field.regMask(), sizeof(DataType) * 2,
                         op.msg);
            }
            #endif
        }, op.address);
    }
    virtual void end(std::string_view td, std::string_view ti, Operations::PollRead<AddressType, DataType> const& op) override
    {
        this->end(td, ti);
    }
    virtual void error(std::string_view td, std::string_view ti, std::exception const& ex) override
    {
        if (this->consolidated)
            this->consolidated->error(td, ti, ex.what());
    }

private:
    IConsolidatedFluentInterposer* consolidated;
    OperationFormattingVerbosity verbosity;
    size_t array_size_limit;
};

template <template<typename, typename> typename BaseInterposerType = BasicFluentInterposerAdapter, typename... Args>
inline
void createDefaultInterposerAdapters(Args... args)
{
    RTF::IFluentInterposer<uint8_t, uint8_t>::setDefault(std::make_unique<BaseInterposerType<uint8_t, uint8_t>>(args...));
    RTF::IFluentInterposer<uint8_t, uint16_t>::setDefault(std::make_unique<BaseInterposerType<uint8_t, uint16_t>>(args...));
    RTF::IFluentInterposer<uint8_t, uint32_t>::setDefault(std::make_unique<BaseInterposerType<uint8_t, uint32_t>>(args...));
    RTF::IFluentInterposer<uint8_t, uint64_t>::setDefault(std::make_unique<BaseInterposerType<uint8_t, uint64_t>>(args...));
    RTF::IFluentInterposer<uint16_t, uint8_t>::setDefault(std::make_unique<BaseInterposerType<uint16_t, uint8_t>>(args...));
    RTF::IFluentInterposer<uint16_t, uint16_t>::setDefault(std::make_unique<BaseInterposerType<uint16_t, uint16_t>>(args...));
    RTF::IFluentInterposer<uint16_t, uint32_t>::setDefault(std::make_unique<BaseInterposerType<uint16_t, uint32_t>>(args...));
    RTF::IFluentInterposer<uint16_t, uint64_t>::setDefault(std::make_unique<BaseInterposerType<uint16_t, uint64_t>>(args...));
    RTF::IFluentInterposer<uint32_t, uint8_t>::setDefault(std::make_unique<BaseInterposerType<uint32_t, uint8_t>>(args...));
    RTF::IFluentInterposer<uint32_t, uint16_t>::setDefault(std::make_unique<BaseInterposerType<uint32_t, uint16_t>>(args...));
    RTF::IFluentInterposer<uint32_t, uint32_t>::setDefault(std::make_unique<BaseInterposerType<uint32_t, uint32_t>>(args...));
    RTF::IFluentInterposer<uint32_t, uint64_t>::setDefault(std::make_unique<BaseInterposerType<uint32_t, uint64_t>>(args...));
    RTF::IFluentInterposer<uint64_t, uint8_t>::setDefault(std::make_unique<BaseInterposerType<uint64_t, uint8_t>>(args...));
    RTF::IFluentInterposer<uint64_t, uint16_t>::setDefault(std::make_unique<BaseInterposerType<uint64_t, uint16_t>>(args...));
    RTF::IFluentInterposer<uint64_t, uint32_t>::setDefault(std::make_unique<BaseInterposerType<uint64_t, uint32_t>>(args...));
    RTF::IFluentInterposer<uint64_t, uint64_t>::setDefault(std::make_unique<BaseInterposerType<uint64_t, uint64_t>>(args...));
}

template <typename T>
class OwnedOrViewedObject final
{
public:
    OwnedOrViewedObject(T* viewed_obj) : object(viewed_obj) {}
    OwnedOrViewedObject(std::unique_ptr<T> owned_obj) : object(std::move(owned_obj)) {}
    OwnedOrViewedObject(std::shared_ptr<T> shared_obj) : object(std::move(shared_obj)) {}
    T& operator*() const
    {
        return std::visit([](auto&& obj) {
            using U = std::decay_t<decltype(obj)>;
            if constexpr (std::is_same_v<U, T*>) {
                return &obj;
            }
            else {
                return *obj;
            }
        }, this->object);
    }
    T* operator->() const
    {
        return std::visit([](auto&& obj) {
            using U = std::decay_t<decltype(obj)>;
            if constexpr (std::is_same_v<U, T*>) {
                return obj;
            }
            else {
                return obj.get();
            }
        }, this->object);
    }
private:
    std::variant<T*, std::unique_ptr<T>, std::shared_ptr<T>> object;
};

class WriteVerifyFailureException : public std::runtime_error
{
public:
    template <ValidAddressOrDataType DataType>
    WriteVerifyFailureException(DataType expected, DataType mask, DataType full_actual)
        : std::runtime_error(std::format("WriteVerify mismatch! Expected:0x{:0{}x} Got:0x{:0{}x} (0x{:0{}x})", expected, sizeof(DataType) * 2, full_actual & mask, sizeof(DataType) * 2, full_actual, sizeof(DataType) * 2))
    {}
};
class ReadVerifyFailureException : public std::runtime_error
{
public:
    template <ValidAddressOrDataType DataType>
    ReadVerifyFailureException(DataType expected, DataType mask, DataType full_actual)
        : std::runtime_error(std::format("ReadVerify mismatch! Expected:0x{:0{}x} Got:0x{:0{}x} (0x{:0{}x})", expected, sizeof(DataType) * 2, full_actual & mask, sizeof(DataType) * 2, full_actual, sizeof(DataType) * 2))
    {}
};
class PollReadTimeoutException : public std::runtime_error
{
public:
    template <ValidAddressOrDataType DataType>
    PollReadTimeoutException(DataType expected, DataType mask, DataType full_actual)
        : std::runtime_error(std::format("PollRead timeout! Expected:0x{:0{}x} Got:0x{:0{}x} (0x{:0{}x})", expected, sizeof(DataType) * 2, full_actual & mask, sizeof(DataType) * 2, full_actual, sizeof(DataType) * 2))
    {}
};

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
class Fluent //: public IRegisterTarget<AddressType, DataType> // Can't actually inherit because of covariance requirements on return values.
{
private:
    template <typename OpT, typename OpFunctor>
    Fluent& doOp(OpT const& op, OpFunctor fn)
    {
        if (this->interposer)
            this->interposer->op(this->getDomain(), this->getName(), op);
        try {
            fn();
        }
        catch (std::exception const& ex) {
            if (this->interposer)
                this->interposer->error(this->getDomain(), this->getName(), ex);
            throw;
        }
        if (this->interposer)
            this->interposer->end(this->getDomain(), this->getName(), op);
        return *this;
    }
public:
    Fluent(IFluentInterposer<AddressType, DataType>* interposer, IRegisterTarget<AddressType, DataType>& target)
        : interposer(interposer)
        , target(&target)
    {}
    explicit Fluent(IRegisterTarget<AddressType, DataType>& target)
        : Fluent(IFluentInterposer<AddressType, DataType>::getDefault(), target)
    {}

    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    Fluent(IFluentInterposer<AddressType, DataType>* interposer, std::unique_ptr<T> target)
        : interposer(interposer)
        , target(std::unique_ptr<IRegisterTarget<AddressType, DataType>>(std::move(target)))
    {}
    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    explicit Fluent(std::unique_ptr<T> target)
        : Fluent(IFluentInterposer<AddressType, DataType>::getDefault(), std::move(target))
    {}

    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    Fluent(IFluentInterposer<AddressType, DataType>* interposer, std::shared_ptr<T> target)
        : interposer(interposer)
        , target(std::shared_ptr<IRegisterTarget<AddressType, DataType>>(std::move(target)))
    {}
    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    explicit Fluent(std::shared_ptr<T> target)
        : Fluent(IFluentInterposer<AddressType, DataType>::getDefault(), std::move(target))
    {}

    std::string_view getDomain() const { return this->target->getDomain(); }
    std::string_view getName() const { return this->target->getName(); }

    template <typename... Args>
    Fluent& seq(std::format_string<Args...> fmt, Args... args)
    {
        if (this->interposer)
            this->interposer->seq(this->getDomain(), this->getName(), Operations::Seq{std::vformat(fmt.get(), std::make_format_args(args...))});
        return *this;
    }
    Fluent& seq(std::string_view msg)
    {
        if (this->interposer)
            this->interposer->seq(this->getDomain(), this->getName(), Operations::Seq{msg});
        return *this;
    }

    template <typename... Args>
    Fluent& step(std::format_string<Args...> fmt, Args... args)
    {
        if (this->interposer)
            this->interposer->step(this->getDomain(), this->getName(), Operations::Step{std::vformat(fmt.get(), std::make_format_args(args...))});
        return *this;
    }
    Fluent& step(std::string_view msg)
    {
        if (this->interposer)
            this->interposer->step(this->getDomain(), this->getName(), Operations::Step{msg});
        return *this;
    }

    Fluent& null(std::string_view msg = "")
    {
        auto const op = Operations::Null{msg};
        return doOp(op, [] { /* do nothing */});

    }

    Fluent& delay(std::chrono::microseconds delay, std::string_view msg = "")
    {
        auto const op = Operations::Delay{delay, msg};
        return doOp(op, [&] {
            std::this_thread::sleep_for(delay);
        });
    }

    Fluent& write(AddressType addr, DataType data, std::string_view msg = "")
    {
        auto const op = Operations::Write<AddressType, DataType>{addr, data, msg};
        return doOp(op, [&] {
            this->target->write(addr, data);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& write(::RMF::Register<AddressType, DataType> const& reg, DataType data, std::string_view msg = "")
    {
        auto const op = Operations::Write<AddressType, DataType>{reg, data, msg};
        return doOp(op, [&] {
            this->target->write(reg.address(), data);
        });
    }
    #ifdef RTF_ENABLE_POTENTIALLY_MISUSED_OPERATIONS
    Fluent& write(::RMF::Field<AddressType, DataType> const& field, DataType field_data, std::string_view msg = "")
    {
        auto const op = Operations::Write<AddressType, DataType>{field, field.regVal(field_data), msg};
        return doOp(op, [&] {
            this->target->write(field.address(), field.regVal(field_data));
        });
    }
    #else
    Fluent& write(::RMF::Field<AddressType, DataType> const& field, DataType field_data, std::string_view msg = "") = delete;
    #endif
    #endif

    Fluent& read(AddressType addr, DataType& out_data, std::string_view msg = "")
    {
        auto const op = Operations::Read<AddressType, DataType>{addr, out_data, msg};
        return doOp(op, [&] {
            out_data = this->target->read(addr);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& read(::RMF::Register<AddressType, DataType> const& reg, DataType& out_data, std::string_view msg = "")
    {
        auto const op = Operations::Read<AddressType, DataType>{reg, out_data, msg};
        return doOp(op, [&] {
            out_data = this->target->read(reg.address());
        });
    }
    Fluent& read(::RMF::Field<AddressType, DataType> const& field, DataType& out_field_data, std::string_view msg = "")
    {
        DataType reg_data{};
        auto const op = Operations::Read<AddressType, DataType>{field, reg_data, msg};
        return doOp(op, [&] {
            reg_data = this->target->read(field.address());
            out_field_data = field.extract(reg_data);
        });
    }
    #endif

    Fluent& readModifyWrite(AddressType addr, DataType new_data, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::ReadModifyWrite<AddressType, DataType>{addr, new_data, mask, msg};
        return doOp(op, [&] {
            this->target->readModifyWrite(addr, new_data, mask);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& readModifyWrite(::RMF::Register<AddressType, DataType> const& reg, DataType new_data, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::ReadModifyWrite<AddressType, DataType>{reg, new_data, mask, msg};
        return doOp(op, [&] {
            this->target->readModifyWrite(reg.address(), new_data, mask);
        });
    }
    Fluent& readModifyWrite(::RMF::Field<AddressType, DataType> const& field, DataType field_new_data, std::string_view msg = "")
    {
        auto const op = Operations::ReadModifyWrite<AddressType, DataType>{field, field.regVal(field_new_data), field.dataMask(), msg};
        return doOp(op, [&] {
            this->target->readModifyWrite(field.address(), field.regVal(field_new_data), field.regMask());
        });
    }
    #endif

    Fluent& seqWrite(AddressType start_addr, std::span<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        auto const op = Operations::SeqWrite<AddressType, DataType>{start_addr, data, increment, msg};
        return doOp(op, [&] {
            this->target->seqWrite(start_addr, data, increment);
        });
    }
    Fluent& seqRead(AddressType start_addr, std::span<DataType> out_data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        auto const op = Operations::SeqRead<AddressType, DataType>{start_addr, out_data, increment, msg};
        return doOp(op, [&] {
            this->target->seqRead(start_addr, out_data, increment);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& seqWrite(::RMF::Register<AddressType, DataType> const& start_reg, std::span<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        auto const op = Operations::SeqWrite<AddressType, DataType>{start_reg, data, increment, msg};
        return doOp(op, [&] {
            this->target->seqWrite(start_reg.address(), data, increment);
        });
    }
    Fluent& seqRead(::RMF::Register<AddressType, DataType> const& start_reg, std::span<DataType> out_data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        auto const op = Operations::SeqRead<AddressType, DataType>{start_reg, out_data, increment, msg};
        return doOp(op, [&] {
            this->target->seqRead(start_reg.address(), out_data, increment);
        });
    }
    #endif

    Fluent& fifoWrite(AddressType fifo_addr, std::span<DataType const> data, std::string_view msg = "")
    {
        auto const op = Operations::FifoWrite<AddressType, DataType>{fifo_addr, data, msg};
        return doOp(op, [&] {
            this->target->fifoWrite(fifo_addr, data);
        });
    }
    Fluent& fifoRead(AddressType fifo_addr, std::span<DataType> out_data, std::string_view msg = "")
    {
        auto const op = Operations::FifoRead<AddressType, DataType>{fifo_addr, out_data, msg};
        return doOp(op, [&] {
            this->target->fifoRead(fifo_addr, out_data);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& fifoWrite(::RMF::Register<AddressType, DataType> const& fifo_reg, std::span<DataType const> data, std::string_view msg = "")
    {
        auto const op = Operations::FifoWrite<AddressType, DataType>{fifo_reg, data, msg};
        return doOp(op, [&] {
            this->target->fifoWrite(fifo_reg.address(), data);
        });
    }
    Fluent& fifoRead(::RMF::Register<AddressType, DataType> const& fifo_reg, std::span<DataType> out_data, std::string_view msg = "")
    {
        auto const op = Operations::FifoRead<AddressType, DataType>{fifo_reg, out_data, msg};
        return doOp(op, [&] {
            this->target->fifoRead(fifo_reg.address(), out_data);
        });
    }
    #endif

    Fluent& compWrite(std::span<std::pair<AddressType, DataType> const> addr_data, std::string_view msg = "")
    {
        auto const op = Operations::CompWrite{addr_data, msg};
        return doOp(op, [&] {
            this->target->compWrite(addr_data);
        });
    }
    Fluent& compRead(std::span<AddressType const> const addresses, std::span<DataType> out_data, std::string_view msg = "")
    {
        auto const op = Operations::CompRead<AddressType, DataType>{addresses, out_data, msg};
        return doOp(op, [&] {
            this->target->compRead(addresses, out_data);
        });
    }

    Fluent& writeVerify(AddressType addr, DataType data, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::WriteVerify(addr, data, mask, msg);
        return doOp(op, [&] {
            this->target->write(addr, data);
            DataType const reg_val = this->target->read(addr);
            DataType const expected_val = data & mask;
            if ((reg_val & mask) != expected_val)
                throw WriteVerifyFailureException(expected_val, mask, reg_val);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& writeVerify(::RMF::Register<AddressType, DataType> const& reg, DataType data, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::WriteVerify<AddressType, DataType>{reg, data, mask, msg};
        return doOp(op, [&] {
            this->target->write(reg.address(), data);
            DataType const reg_val = this->target->read(reg.address());
            DataType const expected_val = data & mask;
            if ((reg_val & mask) != expected_val)
                throw WriteVerifyFailureException(expected_val, mask, reg_val);
        });
    }
    #ifdef RTF_ENABLE_POTENTIALLY_MISUSED_OPERATIONS
    Fluent& writeVerify(::RMF::Field<AddressType, DataType> const& field, DataType field_data, std::string_view msg = "")
    {
        auto const op = Operations::WriteVerify<AddressType, DataType>{field, field.regVal(field_data), field.dataMask(), msg};
        return doOp(op, [&] {
            DataType const data = field.regVal(field_data);
            this->target->write(field.address(), data);
            DataType const reg_val = this->target->read(field.address());
            DataType const mask = field.regMask();
            DataType const expected_val = data & mask;
            if ((reg_val & mask) != expected_val)
                throw WriteVerifyFailureException(expected_val, mask, reg_val);
        });
    }
    #else
    Fluent& writeVerify(::RMF::Field<AddressType, DataType> const& field, DataType field_data, std::string_view msg = "") = delete;
    #endif
    #endif

    Fluent& readVerify(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::ReadVerify{addr, expected, mask, msg};
        return doOp(op, [&] {
            DataType const reg_val = this->target->read(addr);
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw ReadVerifyFailureException(expected_val, mask, reg_val);
        });
    }

    #ifdef RTF_INTEROP_RMF
    Fluent& readVerify(::RMF::Register<AddressType, DataType> const& reg, DataType expected, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::ReadVerify<AddressType, DataType>{reg, expected, mask, msg};
        return doOp(op, [&] {
            DataType const reg_val = this->target->read(reg.address());
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw ReadVerifyFailureException(expected_val, mask, reg_val);
        });
    }
    Fluent& readVerify(::RMF::Field<AddressType, DataType> const& field, DataType field_expected, std::string_view msg = "")
    {
        auto const op = Operations::ReadVerify<AddressType, DataType>{field, field.regVal(field_expected), field.dataMask(), msg};
        return doOp(op, [&] {
            DataType const expected = field.regVal(field_expected);
            DataType const mask = field.regMask();
            DataType const reg_val = this->target->read(field.address());
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw ReadVerifyFailureException(expected_val, mask, reg_val);
        });
    }
    #endif

    template <CPoller PollerType>
    Fluent& pollRead(PollerType const &poller, AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::PollRead<AddressType, DataType>{addr, expected, mask, msg};
        return doOp(op, [&] {
            DataType const expected_val = expected & mask;
            DataType reg_val = {};
            bool const success = poller([&] {
                reg_val = this->target->read(addr);
                return (reg_val & mask) == expected_val;
            });
            if (!success)
                throw PollReadTimeoutException(expected_val, mask, reg_val);
        });
    }
    Fluent& pollRead(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        return this->pollRead(default_poller, addr, expected, mask, msg);
    }

    #ifdef RTF_INTEROP_RMF
    template <CPoller PollerType>
    Fluent& pollRead(PollerType const& poller, ::RMF::Register<AddressType, DataType> const& reg, DataType expected, DataType mask, std::string_view msg = "")
    {
        auto const op = Operations::PollRead<AddressType, DataType>{reg, expected, mask, msg};
        return doOp(op, [&] {
            DataType const expected_val = expected & mask;
            DataType reg_val = {};
            bool const success = poller([&] {
                reg_val = this->target->read(reg.address());
                return (reg_val & mask) == expected_val;
            });
            if (!success)
                throw PollReadTimeoutException(expected_val, mask, reg_val);
        });
    }
    Fluent& pollRead(::RMF::Register<AddressType, DataType> const& reg, DataType expected, DataType mask, std::string_view msg = "")
    {
        return this->pollRead(default_poller, reg, expected, mask, msg);
    }

    template <CPoller PollerType>
    Fluent& pollRead(PollerType const& poller, ::RMF::Field<AddressType, DataType> const& field, DataType field_expected, std::string_view msg = "")
    {
        auto const op = Operations::PollRead<AddressType, DataType>{field, field.regVal(field_expected), field.dataMask(), msg};
        return doOp(op, [&] {
            DataType const expected = field.regVal(field_expected);
            DataType const mask = field.regMask();
            DataType const expected_val = expected & mask;
            DataType reg_val = {};
            bool const success = poller([&] {
                reg_val = this->target->read(field.address());
                return (reg_val & mask) == expected_val;
            });
            if (!success)
                throw PollReadTimeoutException(expected_val, mask, reg_val);
        });
    }
    Fluent& pollRead(::RMF::Field<AddressType, DataType> const& field, DataType field_expected, std::string_view msg = "")
    {
        return this->pollRead(default_poller, field, field_expected, msg);
    }
    #endif

    // Overloads that read data and return it instead of using out parameters
    [[nodiscard]] DataType read(AddressType addr, std::string_view msg = "")
    {
        DataType out_data;
        this->read(addr, out_data, msg);
        return out_data;
    }
    [[nodiscard]] std::vector<DataType> seqRead(AddressType start_addr, size_t count, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(count);
        this->seqRead(start_addr, rv, increment, msg);
        return rv;
    }
    [[nodiscard]] std::vector<DataType> fifoRead(AddressType fifo_addr, size_t count, std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(count);
        this->fifoRead(fifo_addr, rv, msg);
        return rv;
    }
    [[nodiscard]] std::vector<DataType> compRead(std::span<AddressType const> const addresses, std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(addresses.size());
        this->compRead(addresses, rv, msg);
        return rv;
    }
    #ifdef RTF_INTEROP_RMF
    [[nodiscard]] DataType read(::RMF::Register<AddressType, DataType> const& reg, std::string_view msg = "")
    {
        DataType out_data;
        this->read(reg, out_data, msg);
        return out_data;
    }
    [[nodiscard]] DataType read(::RMF::Field<AddressType, DataType> const& field, std::string_view msg = "")
    {
        DataType out_regval;
        this->read(field, out_regval, msg);
        return out_regval;
    }
    [[nodiscard]] std::vector<DataType> seqRead(::RMF::Register<AddressType, DataType> const& start_reg, size_t count, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(count);
        this->seqRead(start_reg, rv, increment, msg);
        return rv;
    }
    [[nodiscard]] std::vector<DataType> fifoRead(::RMF::Register<AddressType, DataType> const& fifo_reg, size_t count, std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(count);
        this->fifoRead(fifo_reg, rv, msg);
        return rv;
    }
    #endif

    // Overloads that take a std::initializer_list instead of std::span (see P2447, adopted into C++26, so in a decade these can be removed!)
    Fluent& seqWrite(AddressType start_addr, std::initializer_list<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        return this->seqWrite(start_addr, std::span{ data.begin(), data.end() }, increment, msg);
    }
    Fluent& fifoWrite(AddressType fifo_addr, std::initializer_list<DataType const> data, std::string_view msg = "")
    {
        return this->fifoWrite(fifo_addr, std::span{ data.begin(), data.end() }, msg);
    }
    Fluent& compWrite(std::initializer_list<std::pair<AddressType, DataType> const> addr_data, std::string_view msg = "")
    {
        return this->compWrite(std::span{ addr_data.begin(), addr_data.end() }, msg);
    }
    Fluent& compRead(std::initializer_list<AddressType const> const addresses, std::span<DataType> out_data, std::string_view msg = "")
    {
        return this->compRead(std::span{ addresses.begin(), addresses.end() }, out_data, msg);
    }
    [[nodiscard]] std::vector<DataType> compRead(std::initializer_list<AddressType const> const addresses, std::string_view msg = "")
    {
        return this->compRead(std::span{ addresses.begin(), addresses.end() }, msg);
    }
    #ifdef RTF_INTEROP_RMF
    Fluent& seqWrite(::RMF::Register<AddressType, DataType> const& start_reg, std::initializer_list<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        return this->seqWrite(start_reg, std::span{ data.begin(), data.end() }, increment, msg);
    }
    Fluent& fifoWrite(::RMF::Register<AddressType, DataType> const& fifo_reg, std::initializer_list<DataType const> data, std::string_view msg = "")
    {
        return this->fifoWrite(fifo_reg, std::span{ data.begin(), data.end() }, msg);
    }
    #endif

private:
    IFluentInterposer<AddressType, DataType>* interposer;
    OwnedOrViewedObject<IRegisterTarget<AddressType, DataType>> target;
};

template <typename T>
Fluent(std::shared_ptr<T>) -> Fluent<typename T::AddressType, typename T::DataType>;
template <typename T>
Fluent(IFluentInterposer<typename T::AddressType, typename T::DataType>*, std::shared_ptr<T>) -> Fluent<typename T::AddressType, typename T::DataType>;
template <typename T>
Fluent(std::unique_ptr<T>) -> Fluent<typename T::AddressType, typename T::DataType>;
template <typename T>
Fluent(IFluentInterposer<typename T::AddressType, typename T::DataType>*, std::unique_ptr<T>) -> Fluent<typename T::AddressType, typename T::DataType>;

template <typename T, typename FnType>
inline
void chunkify(std::span<T> buffer, size_t max_chunk_size, FnType fn)
{
    for (size_t pos = 0 ; pos < buffer.size() ; ){
        auto const chunk_size = std::min(max_chunk_size, buffer.size() - pos);
        auto const chunk = buffer.subspan(pos, chunk_size);
        fn(chunk, pos);
        pos += chunk_size;
    }
}

#ifndef BIT
#ifndef RTF_NO_BIT
#define BIT(nr) (1ULL << (nr))
#endif
#endif

}

