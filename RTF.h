// Copyright (c) 2024 Matt M Halenza
// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
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

namespace RTF {

#ifndef RTF_UNRESTRICTED_ADDRESS_AND_DATA_TYPES
template <typename T>
concept ValidAddressOrDataType = std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>;
#else
template <typename T>
concept ValidAddressOrDataType = std::is_same_v<T, T>;
#endif

template <ValidAddressOrDataType AddressType, ValidAddressOrDataType DataType>
struct IRegisterTarget
{
protected:
    IRegisterTarget() = default;
public:
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
    void opExtra(std::span<std::pair<AddressType,DataType> const> addr_data)
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

    FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, std::unique_ptr<IRegisterTarget<AddressType, DataType>> target)
        : interposer(interposer)
        , target(std::move(target))
    {}
    explicit FluentRegisterTarget(std::unique_ptr<IRegisterTarget<AddressType, DataType>> target)
        : FluentRegisterTarget(IFluentRegisterTargetInterposer::getDefault(), target)
    {}

    FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, std::shared_ptr<IRegisterTarget<AddressType, DataType>> target)
        : interposer(interposer)
        , target(std::move(target))
    {}
    explicit FluentRegisterTarget(std::shared_ptr<IRegisterTarget<AddressType, DataType>> target)
        : FluentRegisterTarget(IFluentRegisterTargetInterposer::getDefault(), target)
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
                throw std::runtime_error(std::format("WriteVerify mismatch! Expected:0x{:0{}x} Got:0x{:0{}x} (0x{:0{}x})", expected_val, sizeof(DataType) * 2, reg_val & mask, sizeof(DataType) * 2, reg_val, sizeof(DataType) * 2));
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }

    FluentRegisterTarget& readVerify(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
    {
        this->opStart("ReadVerify(0x{:0{}x}, 0x{:0{}x}, 0x{:0{}x}): {}", addr, sizeof(AddressType) * 2, expected, sizeof(DataType) * 2, mask, sizeof(DataType) * 2, msg);
        try{
            DataType const reg_val = this->target->read(addr);
            DataType const expected_val = expected & mask;
            if ((reg_val & mask) != expected_val)
                throw std::runtime_error(std::format("ReadVerify mismatch! Expected:0x{:0{}x} Got:0x{:0{}x} (0x{:0{}x})", expected_val, sizeof(DataType) * 2, reg_val & mask, sizeof(DataType) * 2, reg_val, sizeof(DataType) * 2));
        }
        catch (std::exception const& ex) {
            this->opError(ex.what());
            throw;
        }
        this->opEnd();
        return *this;
    }

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
                throw std::runtime_error(std::format("PollRead timeout! Expected:0x{:0{}x} Last:0x{:0{}x} (0x{:0{}x})", expected_val, sizeof(DataType) * 2, reg_val & mask, sizeof(DataType) * 2, reg_val, sizeof(DataType) * 2));
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
        static BasicPoller const poller = {
            std::chrono::seconds(0),
            std::chrono::microseconds(500),
            std::chrono::seconds(3)
        };
        return this->pollRead(poller, addr, expected, mask, msg);
    }

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

private:
    IFluentRegisterTargetInterposer* interposer;
    OwnedOrViewedObject<IRegisterTarget<AddressType, DataType>> target;
};

}
