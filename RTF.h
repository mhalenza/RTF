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

#ifndef RTF_UNRESTRICTED_ADDRESS_AND_DATA_TYPES
template <typename T>
concept ValidAddressOrDataType = std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>;
#else
template <typename T>
concept ValidAddressOrDataType = std::is_same_v<T, T>;
#endif

template <ValidAddressOrDataType AddressType_, ValidAddressOrDataType DataType_>
struct IRegisterTarget
{
protected:
    IRegisterTarget() = default;
public:
    using AddressType = AddressType_;
    using DataType = DataType_;
    virtual ~IRegisterTarget() = default;

    virtual std::string_view getName() const { return "<unknown>"; }
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
            this->write(start_addr + (increment * i), data[i]);
        }
    }
    virtual void seqRead(AddressType start_addr, std::span<DataType> out_data, size_t increment = sizeof(DataType))
    {
        for (size_t i = 0 ; i < out_data.size() ; i++) {
            out_data[i] = this->read(start_addr + (increment * i));
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

struct IFluentRegisterTargetInterposer
{
protected:
    IFluentRegisterTargetInterposer() = default;
public:
    virtual ~IFluentRegisterTargetInterposer() = default;

    virtual void seq(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void step(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
    virtual void opStart(std::string_view target_domain, std::string_view target_instance, std::string_view op_msg) = 0;
    virtual void opExtra(std::string_view target_domain, std::string_view target_instance, std::string_view values) = 0;
    virtual void opEnd(std::string_view target_domain, std::string_view target_instance) = 0;
    virtual void opError(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;

public:
    static void setDefault(std::unique_ptr<IFluentRegisterTargetInterposer> new_default_interposer)
    {
        default_interposer = std::move(new_default_interposer);
    }
    static IFluentRegisterTargetInterposer* getDefault()
    {
        return default_interposer.get();
    }
private:
    static std::unique_ptr<IFluentRegisterTargetInterposer> default_interposer;
};
#ifdef RTF_IMPLEMENTATION
std::unique_ptr<IFluentRegisterTargetInterposer> IFluentRegisterTargetInterposer::default_interposer = nullptr;
#endif

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
class FluentRegisterTarget //: public IRegisterTarget<AddressType, DataType> // Can't actually inherit because of covariance requirements on return values.
{
private:
    void opStart(std::string_view msg)
    {
        if (this->interposer) {
            this->interposer->opStart("FluentRegisterTarget", this->target->getName(), msg);
        }
    }
    template <typename... Args>
    void opStart(std::format_string<Args...> fmt, Args... args)
    {
        if (this->interposer) {
            this->interposer->opStart("FluentRegisterTarget", this->target->getName(), std::vformat(fmt.get(), std::make_format_args(args...)));
        }
    }
    void opExtra(DataType data)
    {
        if (this->interposer) {
            this->interposer->opExtra("FluentRegisterTarget", this->target->getName(), std::format("0x{:0{}x}", data, sizeof(DataType) * 2));
        }
    }
    void opExtra(std::span<DataType const> data)
    {
        if (this->interposer) {
            for (auto const d : data) {
                this->interposer->opExtra("FluentRegisterTarget", this->target->getName(), std::format("0x{:0{}x}", d, sizeof(DataType) * 2));
            }
        }
    }
    void opExtra(std::span<AddressType const> addresses)
        requires (!std::is_same_v<AddressType, DataType>)
    {
        if (this->interposer) {
            for (auto const a : addresses) {
                this->interposer->opExtra("FluentRegisterTarget", this->target->getName(), std::format("0x{:0{}x}", a, sizeof(AddressType) * 2));
            }
        }
    }
    void opExtra(std::span<std::pair<AddressType, DataType> const> addr_data)
    {
        if (this->interposer) {
            for (auto const ad : addr_data) {
                this->interposer->opExtra("FluentRegisterTarget", this->target->getName(), std::format("0x{:0{}x} 0x{:0{}x}", ad.first, sizeof(AddressType) * 2, ad.second, sizeof(DataType) * 2));
            }
        }
    }
    void opEnd()
    {
        if (this->interposer) {
            this->interposer->opEnd("FluentRegisterTarget", this->target->getName());
        }
    }
    void opError(std::string_view msg)
    {
        if (this->interposer) {
            this->interposer->opError("FluentRegisterTarget", this->target->getName(), msg);
        }
    }
public:
    FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, IRegisterTarget<AddressType, DataType>& target)
        : interposer(interposer)
        , target(&target)
    {}
    explicit FluentRegisterTarget(IRegisterTarget<AddressType, DataType>& target)
        : FluentRegisterTarget(IFluentRegisterTargetInterposer::getDefault(), target)
    {}

    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, std::unique_ptr<T> target)
        : interposer(interposer)
        , target(std::unique_ptr<IRegisterTarget<AddressType, DataType>>(std::move(target)))
    {}
    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    explicit FluentRegisterTarget(std::unique_ptr<T> target)
        : FluentRegisterTarget(IFluentRegisterTargetInterposer::getDefault(), std::move(target))
    {}

    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, std::shared_ptr<T> target)
        : interposer(interposer)
        , target(std::shared_ptr<IRegisterTarget<AddressType, DataType>>(std::move(target)))
    {}
    template <std::derived_from<IRegisterTarget<AddressType, DataType>> T>
    explicit FluentRegisterTarget(std::shared_ptr<T> target)
        : FluentRegisterTarget(IFluentRegisterTargetInterposer::getDefault(), std::move(target))
    {}

    template <typename... Args>
    FluentRegisterTarget& seq(std::format_string<Args...> fmt, Args... args)
    {
        if (this->interposer) {
            this->interposer->seq("FluentRegisterTarget", this->target->getName(), std::vformat(fmt.get(), std::make_format_args(args...)));
        }
        return *this;
    }
    FluentRegisterTarget& seq(std::string_view msg)
    {
        if (this->interposer) {
            this->interposer->seq("FluentRegisterTarget", this->target->getName(), msg);
        }
        return *this;
    }

    template <typename... Args>
    FluentRegisterTarget& step(std::format_string<Args...> fmt, Args... args)
    {
        if (this->interposer) {
            this->interposer->step("FluentRegisterTarget", this->target->getName(), std::vformat(fmt.get(), std::make_format_args(args...)));
        }
        return *this;
    }
    FluentRegisterTarget& step(std::string_view msg)
    {
        if (this->interposer) {
            this->interposer->step("FluentRegisterTarget", this->target->getName(), msg);
        }
        return *this;
    }

    FluentRegisterTarget& null(std::string_view msg = "")
    {
        this->opStart("Null(): {}", msg);
        this->opEnd();
        return *this;
    }

    FluentRegisterTarget& delay(std::chrono::microseconds delay, std::string_view msg = "")
    {
        this->opStart("Delay({}): {}", delay, msg);
        std::this_thread::sleep_for(delay);
        this->opEnd();
        return *this;
    }

    FluentRegisterTarget& write(AddressType addr, DataType data, std::string_view msg = "")
    {
        this->opStart("Write(0x{:0{}x}, 0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, data, sizeof(DataType) * 2, msg);
        try {
            this->target->write(addr, data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    FluentRegisterTarget& write(::RMF::Register<AddressType, DataType> const& reg, DataType data, std::string_view msg = "")
    {
        this->opStart("Write(0x{:0{}x} '{}', 0x{:0{}x}): {}", reg.address(), sizeof(AddressType) * 2, reg.fullName(), data, sizeof(DataType) * 2, msg);
        try {
            this->target->write(reg.address(), data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& write(::RMF::Field<AddressType, DataType> const& field, DataType field_data, std::string_view msg = "") = delete;
    #endif

    FluentRegisterTarget& read(AddressType addr, DataType& out_data, std::string_view msg = "")
    {
        this->opStart("Read(0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, msg);
        try {
            out_data = this->target->read(addr);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    FluentRegisterTarget& read(::RMF::Register<AddressType, DataType> const& reg, DataType& out_data, std::string_view msg = "")
    {
        this->opStart("Read(0x{:0{}x} '{}'): {}", reg.address(), sizeof(AddressType) * 2, reg.fullName(), msg);
        try {
            out_data = this->target->read(reg.address());
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& read(::RMF::Field<AddressType, DataType> const& field, DataType& out_data, std::string_view msg = "")
    {
        this->opStart("Read(0x{:0{}x} '{}'): {}", field.address(), sizeof(AddressType) * 2, field.fullName(), msg);
        try {
            out_data = field.extract(this->target->read(field.address()));
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }
    #endif

    FluentRegisterTarget& readModifyWrite(AddressType addr, DataType new_data, DataType mask, std::string_view msg = "")
    {
        this->opStart("ReadModifyWrite(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, new_data & mask, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try {
            this->target->readModifyWrite(addr, new_data, mask);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    FluentRegisterTarget& readModifyWrite(::RMF::Register<AddressType, DataType> const& reg, DataType new_data, DataType mask, std::string_view msg = "")
    {
        this->opStart("ReadModifyWrite(0x{:0{}x} '{}', 0x{:0{}x}, 0x{:0{}x}): {}", reg.address(), sizeof(AddressType) * 2, reg.fullName(), new_data & mask, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try {
            this->target->readModifyWrite(reg.address(), new_data, mask);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& readModifyWrite(::RMF::Field<AddressType, DataType> const& field, DataType field_new_data, std::string_view msg = "")
    {
        DataType const mask = field.regMask();
        DataType const new_data = field.regVal(field_new_data);
        this->opStart("ReadModifyWrite(0x{:0{}x} '{}', 0x{:0{}x}): {}", field.address(), sizeof(AddressType) * 2, field.fullName(), field_new_data, (field.size() + 3) / 4, msg);
        try {
            this->target->readModifyWrite(field.address(), new_data, mask);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    #endif

    FluentRegisterTarget& seqWrite(AddressType start_addr, std::span<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        this->opStart("SeqWrite(0x{:0{}x}, {}.., {}): {}", start_addr, sizeof(AddressType) * 2, data.size(), increment, msg);
        this->opExtra(data);
        try {
            this->target->seqWrite(start_addr, data, increment);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& seqRead(AddressType start_addr, std::span<DataType> out_data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        this->opStart("SeqRead(0x{:0{}x}, {}.., {}): {}", start_addr, sizeof(AddressType) * 2, out_data.size(), increment, msg);
        try {
            this->target->seqRead(start_addr, out_data, increment);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    #ifdef RTF_ENABLE_POTENTIALLY_MISUSED_OPERATIONS
    FluentRegisterTarget& seqWrite(::RMF::Register<AddressType, DataType> const& start_reg, std::span<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        this->opStart("SeqWrite(0x{:0{}x} '{}', {}.., {}): {}", start_reg.address(), sizeof(AddressType) * 2, start_reg.fullName(), data.size(), increment, msg);
        this->opExtra(data);
        try {
            this->target->seqWrite(start_reg.address(), data, increment);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& seqRead(::RMF::Register<AddressType, DataType> const& start_reg, std::span<DataType> out_data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        this->opStart("SeqRead(0x{:0{}x} '{}', {}.., {}): {}", start_reg.address(), sizeof(AddressType) * 2, start_reg.fullName(), out_data.size(), increment, msg);
        try {
            this->target->seqRead(start_reg.address(), out_data, increment);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }
    #else
    FluentRegisterTarget& seqWrite(::RMF::Register<AddressType, DataType> const& start_reg, std::span<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "") = delete;
    FluentRegisterTarget& seqRead(::RMF::Register<AddressType, DataType> const& start_reg, std::span<DataType> out_data, size_t increment = sizeof(DataType), std::string_view msg = "") = delete;
    #endif
    #endif

    FluentRegisterTarget& fifoWrite(AddressType fifo_addr, std::span<DataType const> data, std::string_view msg = "")
    {
        this->opStart("FifoWrite(0x{:0{}x}, {}..): {}", fifo_addr, sizeof(AddressType) * 2, data.size(), msg);
        this->opExtra(data);
        try {
            this->target->fifoWrite(fifo_addr, data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& fifoRead(AddressType fifo_addr, std::span<DataType> out_data, std::string_view msg = "")
    {
        this->opStart("FifoRead(0x{:0{}x}, {}): {}", fifo_addr, sizeof(AddressType) * 2, out_data.size(), msg);
        try {
            this->target->fifoRead(fifo_addr, out_data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    FluentRegisterTarget& fifoWrite(::RMF::Register<AddressType, DataType> const& fifo_reg, std::span<DataType const> data, std::string_view msg = "")
    {
        this->opStart("FifoWrite(0x{:0{}x} '{}', {}..): {}", fifo_reg.address(), sizeof(AddressType) * 2, fifo_reg.fullName(), data.size(), msg);
        this->opExtra(data);
        try {
            this->target->fifoWrite(fifo_reg.address(), data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& fifoRead(::RMF::Register<AddressType, DataType> const& fifo_reg, std::span<DataType> out_data, std::string_view msg = "")
    {
        this->opStart("FifoRead(0x{:0{}x} '{}', {}): {}", fifo_reg.address(), sizeof(AddressType) * 2, fifo_reg.fullName(), out_data.size(), msg);
        try {
            this->target->fifoRead(fifo_reg.address(), out_data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }
    #endif

    FluentRegisterTarget& compWrite(std::span<std::pair<AddressType, DataType> const> addr_data, std::string_view msg = "")
    {
        this->opStart("CompWrite({}..): {}", addr_data.size(), msg);
        this->opExtra(addr_data);
        try {
            this->target->compWrite(addr_data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& compRead(std::span<AddressType const> const addresses, std::span<DataType> out_data, std::string_view msg = "")
    {
        this->opStart("CompRead({}.., {}..): {}", addresses.size(), out_data.size(), msg);
        this->opExtra(addresses);
        try {
            this->target->compRead(addresses, out_data);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opExtra(out_data);
        this->opEnd();
        return *this;
    }

    FluentRegisterTarget& writeVerify(AddressType addr, DataType data, DataType mask, std::string_view msg = "")
    {
        this->opStart("WriteVerify(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, data, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try {
            this->target->write(addr, data);
            DataType const reg_val = this->target->read(addr);
            DataType const expected_val = data & mask;
            if ((reg_val & mask) != expected_val)
                throw WriteVerifyFailureException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    FluentRegisterTarget& writeVerify(::RMF::Register<AddressType, DataType> const& reg, DataType data, DataType mask, std::string_view msg = "")
    {
        this->opStart("WriteVerify(0x{:0{}x} '{}, 0x{:0{}x}, 0x{:0{}x}): {}", reg.address(), sizeof(AddressType) * 2, reg.fullName(), data, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try {
            this->target->write(reg.address(), data);
            DataType const reg_val = this->target->read(reg.address());
            DataType const expected_val = data & mask;
            if ((reg_val & mask) != expected_val)
                throw WriteVerifyFailureException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& writeVerify(::RMF::Field<AddressType, DataType> const& field, DataType field_data, std::string_view msg = "") = delete;
    #endif

    FluentRegisterTarget& readVerify(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        this->opStart("ReadVerify(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, expected, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try{
            DataType const reg_val = this->target->read(addr);
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw ReadVerifyFailureException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }

    #ifdef RTF_INTEROP_RMF
    FluentRegisterTarget& readVerify(::RMF::Register<AddressType, DataType> const& reg, DataType expected, DataType mask, std::string_view msg = "")
    {
        this->opStart("ReadVerify(0x{:0{}x} '{}', 0x{:0{}x}): {}", reg.address(), sizeof(AddressType) * 2, reg.fullName(), expected, sizeof(DataType) * 2, msg);
        try{
            DataType const reg_val = this->target->read(reg.address());
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw ReadVerifyFailureException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& readVerify(::RMF::Field<AddressType, DataType> const& field, DataType field_expected, std::string_view msg = "")
    {
        DataType const expected = field.regVal(field_expected);
        DataType const mask = field.regMask();
        this->opStart("ReadVerify(0x{:0{}x} '{}', 0x{:0{}x}): {}", field.address(), sizeof(AddressType) * 2, field.fullName(), field_expected, (field.size() + 3) / 4, msg);
        try{
            DataType const reg_val = this->target->read(field.address());
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw ReadVerifyFailureException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    #endif

    template <CPoller PollerType>
    FluentRegisterTarget& pollRead(PollerType const &poller, AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        this->opStart("PollRead(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, expected, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try {
            DataType const expected_val = expected & mask;
            DataType reg_val = {};
            bool const success = poller([&] {
                reg_val = this->target->read(addr) ;
                return (reg_val & mask) == expected_val;
            });
            if (!success)
                throw PollReadTimeoutException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& pollRead(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        return this->pollRead(default_poller, addr, expected, mask, msg);
    }

    #ifdef RTF_INTEROP_RMF
    template <CPoller PollerType>
    FluentRegisterTarget& pollRead(PollerType const& poller, ::RMF::Register<AddressType, DataType> const& reg, DataType expected, DataType mask, std::string_view msg = "")
    {
        this->opStart("PollRead(0x{:0{}x} '{}', 0x{:0{}x}, 0x{:0{}x}): {}", reg.address(), sizeof(AddressType) * 2, reg.fullName(), expected, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try {
            DataType const expected_val = expected & mask;
            DataType reg_val = {};
            bool const success = poller([&] {
                reg_val = this->target->read(reg.address()) ;
                return (reg_val & mask) == expected_val;
            });
            if (!success)
                throw PollReadTimeoutException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& pollRead(::RMF::Register<AddressType, DataType> const& reg, DataType expected, DataType mask, std::string_view msg = "")
    {
        return this->pollRead(default_poller, reg, expected, mask, msg);
    }

    template <CPoller PollerType>
    FluentRegisterTarget& pollRead(PollerType const& poller, ::RMF::Field<AddressType, DataType> const& field, DataType field_expected, std::string_view msg = "")
    {
        DataType const expected = field.regVal(field_expected);
        DataType const mask = field.regMask();
        this->opStart("PollRead(0x{:0{}x} '{}', 0x{:0{}x}): {}", field.address(), sizeof(AddressType) * 2, field.fullName(), field_expected, (field.size() + 3) / 4, msg);
        try {
            DataType const expected_val = expected & mask;
            DataType reg_val = {};
            bool const success = poller([&] {
                reg_val = this->target->read(field.address()) ;
                return (reg_val & mask) == expected_val;
            });
            if (!success)
                throw PollReadTimeoutException(expected_val, mask, reg_val);
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }
    FluentRegisterTarget& pollRead(::RMF::Field<AddressType, DataType> const& field, DataType field_expected, std::string_view msg = "")
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
    #ifdef RTF_ENABLE_POTENTIALLY_MISUSED_OPERATIONS
    [[nodiscard]] std::vector<DataType> seqRead(::RMF::Register<AddressType, DataType> const& start_reg, size_t count, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(count);
        this->seqRead(start_reg, rv, increment, msg);
        return rv;
    }
    #else
    [[nodiscard]] std::vector<DataType> seqRead(::RMF::Register<AddressType, DataType> const& start_reg, size_t count, size_t increment = sizeof(DataType), std::string_view msg = "") = delete;
    #endif
    [[nodiscard]] std::vector<DataType> fifoRead(::RMF::Register<AddressType, DataType> const& fifo_reg, size_t count, std::string_view msg = "")
    {
        std::vector<DataType> rv;
        rv.resize(count);
        this->fifoRead(fifo_reg, rv, msg);
        return rv;
    }
    #endif

    // Overloads that take a std::initializer_list instead of std::span (see P2447, adopted into C++26, so in a decade these can be removed!)
    FluentRegisterTarget& seqWrite(AddressType start_addr, std::initializer_list<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        return this->seqWrite(start_addr, std::span{ data.begin(), data.end() }, increment, msg);
    }
    FluentRegisterTarget& fifoWrite(AddressType fifo_addr, std::initializer_list<DataType const> data, std::string_view msg = "")
    {
        return this->fifoWrite(fifo_addr, std::span{ data.begin(), data.end() }, msg);
    }
    FluentRegisterTarget& compWrite(std::initializer_list<std::pair<AddressType, DataType> const> addr_data, std::string_view msg = "")
    {
        return this->compWrite(std::span{ addr_data.begin(), addr_data.end() }, msg);
    }
    FluentRegisterTarget& compRead(std::initializer_list<AddressType const> const addresses, std::span<DataType> out_data, std::string_view msg = "")
    {
        return this->compRead(std::span{ addresses.begin(), addresses.end() }, out_data, msg);
    }
    [[nodiscard]] std::vector<DataType> compRead(std::initializer_list<AddressType const> const addresses, std::string_view msg = "")
    {
        return this->compRead(std::span{ addresses.begin(), addresses.end() }, msg);
    }
    #ifdef RTF_INTEROP_RMF
    #ifdef RTF_ENABLE_POTENTIALLY_MISUSED_OPERATIONS
    FluentRegisterTarget& seqWrite(::RMF::Register<AddressType, DataType> const& start_reg, std::initializer_list<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")
    {
        return this->seqWrite(start_reg, std::span{ data.begin(), data.end() }, increment, msg);
    }
    #else
    FluentRegisterTarget& seqWrite(::RMF::Register<AddressType, DataType> const& start_reg, std::initializer_list<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "") = delete;
    #endif
    FluentRegisterTarget& fifoWrite(::RMF::Register<AddressType, DataType> const& fifo_reg, std::initializer_list<DataType const> data, std::string_view msg = "")
    {
        return this->fifoWrite(fifo_reg, std::span{ data.begin(), data.end() }, msg);
    }
    #endif

private:
    IFluentRegisterTargetInterposer* interposer;
    OwnedOrViewedObject<IRegisterTarget<AddressType, DataType>> target;
};

template <typename T>
FluentRegisterTarget(std::shared_ptr<T>) -> FluentRegisterTarget<typename T::AddressType, typename T::DataType>;
template <typename T>
FluentRegisterTarget(IFluentRegisterTargetInterposer*, std::shared_ptr<T>) -> FluentRegisterTarget<typename T::AddressType, typename T::DataType>;
template <typename T>
FluentRegisterTarget(std::unique_ptr<T>) -> FluentRegisterTarget<typename T::AddressType, typename T::DataType>;
template <typename T>
FluentRegisterTarget(IFluentRegisterTargetInterposer*, std::unique_ptr<T>) -> FluentRegisterTarget<typename T::AddressType, typename T::DataType>;

}
