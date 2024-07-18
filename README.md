# Register Target Framework

RTF is a C++1? framework for working with register spaces, common in embedded and hardware-centric applications.

## Table of Contents
- [Getting Started](#getting-started)
- [IRegisterTarget](#iregistertarget)
- [FluentRegisterTarget](#fluentregistertarget)
- [IFluentRegisterTargetInterposer](#IFluentRegisterTargetInterposer)
- [CPoller](#cpoller)
- [BasicPoller](#basicpoller)

## Getting Started
RTF is a header-only library, and as such it can simply be copied to your project's source tree.

Exactly one source file must define `RTF_IMPLEMENTATION` before including the header.  
This is to provide storage for a global "interposer" object that will be described later.

Because RTF is a framework, on it's own it doesn't provide much functionality but rather provides useful interface definitions and processes that projects can standardize around.

## IRegisterTarget
`IRegisterTarget` is the main focal point of the library.

The class is templated on two type parameters: `AddressType` and `DataType`.
These template type parameters set the data type used for addresses and data, respectively.
They must be one of: `uint8_t`, `uint16_t`, `uint32_t`, or `uint64_t`.

It provides an interface that represents a physical device that has registers that can be read and written:

```cpp
virtual void write(AddressType addr, DataType data) = 0;
[[nodiscard]] virtual DataType read(AddressType addr) = 0;
```

It is expected that the user's application will define subclasses of this interface for each of the kinds of devices the application will communicate with.
These subclasses must implement these two functions at a minimum.

This interface / abstract base class also provides a number of other member functions for other mechanisms for accessing registers.
These functions are virtual, but not pure.
Therefore, subclasses may choose to not implement them and get the base class behavior (which are simple implementations of the functionality),
OR they may choose to override them and provide a more efficient access method (such as packing multiple writes/reads into a single transaction).

```cpp
virtual void readModifyWrite(AddressType addr, DataType new_data, DataType mask);
```
This function provides a "read-modify-write" mechanism.  `new_data` is bitwise-ANDed with `mask` and then overwrites the portion of the register defined by `mask`.

```cpp
virtual void seqWrite(AddressType start_addr, std::span<DataType const> data, size_t increment = sizeof(DataType));
virtual void seqRead(AddressType start_addr, std::span<DataType> out_data, size_t increment = sizeof(DataType));
```
These functions perform a sequential write or read, starting at address `start_addr`, incrementing the destination address by `increment`.
Data for writes is provided by `data` and the data returned by reads is stored in `out_data` - therefore, the size of these spans encode the number of registers to write.

Subclasses may have restrictions on sequential access, such as only supporting certain `increment` values, only supporting a limited number of accesses in a group, or requiring that the group not span certain address boundaries.
Subclasses *must* check for these unsupported cases and either break them up into supported accesses OR simply defer to the base class implementation (which are always single-accesses in for-loops).

```cpp
virtual void fifoWrite(AddressType fifo_addr, std::span<DataType const> data);
virtual void fifoRead(AddressType fifo_addr, std::span<DataType> out_data);
```
These functions perform a repeated write or read to the same address (given by `fifo_addr`), which is typically an address that writes into or reads from a FIFO.
Data for writes is provided by `data` and the data returned by reads is stored in `out_data` - therefore, the size of these spans encode the number of registers to write.

An observant reader may notice that fifo accesses are logically the same as sequential accesses with an `increement` of `0`.
However, RTF chose to keep them separate in order to facilitate readable code - it would be obvious from the name that these functions write/read a FIFO.

Subclasses may have restrictions on access, such as only being able to pack a limited number of writes/reads into a lower-level access.
Subclasses *must* check for these unsupported cases and either break them up into supported accesses OR simply defer to the base class implementation (which are always single-accesses in for-loops).

```cpp
virtual void compWrite(std::span<std::pair<AddressType, DataType> const> addr_data);
virtual void compRead(std::span<AddressType const> const addresses, std::span<DataType> out_data);
```
These functions perform a "compressed" write or read to a non-contiguous set of addresses.
For writes, the address-data pairs are provided in `addr_data`.
For reads, the addresses are provided in `addresses` and the data read from those registers is stored in `out_data`; the size of the `addresses` and `out_data` spans must be identical or an assert() will fire.

Subclasses may have restrictions on access, such as only being able to pack a limited number of writes/reads into a lower-level access.
Subclasses *must* check for these unsupported cases and either break them up into supported accesses OR simply defer to the base class implementation (which are always single-accesses in for-loops).

## FluentRegisterTarget
`FluentRegisterTarget` provides an API that is modeled after `IRegisterTarget` but is a fluent API and includes even more functionality.
However, it does *not* subclass `IRegisterTarget` due to return value covariance requirements in the C++ language.

The FluentRegisterTarget also includes "Interposer" functionality, which is described in [IFluentRegisterTargetInterposer](#ifluentregistertargetinterposer).

### Construction
There are 6 constructors available:
```cpp
/*1*/          FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, IRegisterTarget<AddressType, DataType>& target);
/*2*/ explicit FluentRegisterTarget(                                             IRegisterTarget<AddressType, DataType>& target);
/*3*/          FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, std::unique_ptr<IRegisterTarget<AddressType, DataType>> target);
/*4*/ explicit FluentRegisterTarget(                                             std::unique_ptr<IRegisterTarget<AddressType, DataType>> target);
/*5*/          FluentRegisterTarget(IFluentRegisterTargetInterposer* interposer, std::shared_ptr<IRegisterTarget<AddressType, DataType>> target);
/*6*/ explicit FluentRegisterTarget(                                             std::shared_ptr<IRegisterTarget<AddressType, DataType>> target);
```

Constructors #1 and #2 take an `IRegisterTarget` by reference and store this reference, effectively "viewing" the real target.
The application must ensure that the `IRegisterTarget` stays alive as long as the `FluentRegisterTarget` is alive.

Constructors #3 and #4 take a `unique_ptr<IRegisterTarget>` and thus take ownership of the real target.

Constructors #5 and #6 take a `shared_ptr<IRegisterTarget>` and thus share ownership of the real target.

Constructors #1, #3, and #5 also take a `IFluentRegisterTargetInterposer*` which is used for interposer operations (described in [IFluentRegisterTargetInterposer](#ifluentregistertargetinterposer)).
The `interposer` argument *may* be `nullptr`, in which case all interposer functionality is skipped.

Constructors #2, #4, and #6 do not take an Interposer argument and instead get a "default" interposer (via `IFluentRegisterTargetInterposer::getDefault()`).

### Sequencing
One aspect to the inerposer functionality is delineating groups of operations.
This is done in two layers: first a "sequence", and then a "step".
Logically, you can think of a sequence being composed of one or more steps, and a step being composed of one or more operations (such as register reads or writes).

Note, however, that this relationship between sequences, steps, and operations is *not* enforced by the fluent API and is merely a tool that can be used to provide context to the operations.

```cpp
FluentRegisterTarget& seq(std::format_string<Args...> fmt, Args... args);
FluentRegisterTarget& seq(std::string_view msg);
FluentRegisterTarget& step(std::format_string<Args...> fmt, Args... args);
FluentRegisterTarget& step(std::string_view msg);
```

### Operations
All operation functions have `std::string_view msg = ""` as the final parameter.
This optional "message" provides context for what the operation is doing and is used only by the interposer and not the execution of the operation itself.

#### Utility
- `null(std::string_view msg = "")`  
    Performs no work and can be used to insert some information into the logs produced by the interposer.
- `delay(std::chrono::microseconds delay, std::string_view msg = "")`  
    Simply puts the calling thread to sleep for the given amount of time.
#### IRegisterTarget Operations
These operations simply call the corresponding function on the contained `IRegisterTarget` (wrapping them with interposer work).
- `write(AddressType addr, DataType data, std::string_view msg = "")`
- `read(AddressType addr, DataType& out_data, std::string_view msg = "")`
- `readModifyWrite(AddressType addr, DataType new_data, DataType mask, std::string_view msg = "")`
- `seqWrite(AddressType start_addr, std::span<DataType const> data, size_t increment = sizeof(DataType), std::string_view msg = "")`
- `seqRead(AddressType start_addr, std::span<DataType> out_data, size_t increment = sizeof(DataType), std::string_view msg = "")`
- `fifoWrite(AddressType fifo_addr, std::span<DataType const> data, std::string_view msg = "")`
- `fifoRead(AddressType fifo_addr, std::span<DataType> out_data, std::string_view msg = "")`
- `compWrite(std::span<std::pair<AddressType, DataType> const> addr_data, std::string_view msg = "")`
- `compRead(std::span<AddressType const> const addresses, std::span<DataType> out_data, std::string_view msg = "")`
#### Verifiers
These functions verify the contents of a register in various ways.  
If the verification fails, the interposer is informed of the failure and then an exception is thrown.
- `writeVerify(AddressType addr, DataType data, DataType mask, std::string_view msg = "")`
    Writes the given value to the register and then reads the register back to verify that the write succeeded.  
    Only the bits in `mask` are actually checked for the verification.
    `data` is bitwise-ANDed with `mask` before writing and the rest of the register is set to zero (ie, it is NOT a read-modify-write operation).
- `readVerify(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")`
    Reads the register and checks that the value read is exactly `expected & mask`.
- `pollRead(PollerType poller, AddressType addr, DataType expected, DataType mask, std::string_view msg = "")`
- `pollRead(AddressType addr, DataType expected, DataType mask, std::string_view msg = "")`
    These functions repeatedly read a register until the value read out exactly matches `expected & mask` or a timeout occurrs.
    The first version uses the given `PollerType` (must conform to the `CPoller` concept) while the second version uses a default `BasicPoller` which times out after 3 seconds.
    See [CPoller](#cpoller) and [BasicPoller](#basicpoller) for more explanation on polling.

In addition to the above functionality, there are overloads for `read()`, `seqRead()`, `fifoRead()`, and `compRead()` that return the register data from the function instead of `FluentRegisterTarget&`.
For `read()` the return type is `DataType`, while for the other three the return type is `std::vector<DataType>`.

Additionally, there are overloads for `seqWrite()`, `fifoWrite()`, `compWrite()` and `compRead()` that substitute `std::span` for `std::initializer_list` for some arguments.
This is due to a flaw in the language where a std::span cannot be constructed from an initializer list.
It was fixed in P2447, adopted into C++26, so once that becomes standard these overloads can be removed.

## IFluentRegisterTargetInterposer
The interposer is a mechanism that hooks into FluentRegisterTarget to provide logging around the operations.

The interface API (that subclasses are expected to implement) is as follows:
```cpp
virtual void seq(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
virtual void step(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
virtual void opStart(std::string_view target_domain, std::string_view target_instance, std::string_view op_msg) = 0;
virtual void opExtra(std::string_view target_domain, std::string_view target_instance, std::string_view values) = 0;
virtual void opEnd(std::string_view target_domain, std::string_view target_instance) = 0;
virtual void opError(std::string_view target_domain, std::string_view target_instance, std::string_view msg) = 0;
```

All of these callbacks have two initial string_view parameters: `target_domain` and `target_instance`.
These values come from the underlying `IRegisterTarget` by calling it's `getDomain()` and `getName()` member functions.
This is to facilitiate identifying what target is being accessed (instance) as well as what kind of device it is (domain).

`seq()` is called in response to `FuentRegisterTarget::seq()`.  
`step()` is called in response to `FluentRegisterTarget::step()`.

The other 4 are called in a defined sequence in response to operations.  
- `opStart()` is always called at the beginning of the operation execution.
- `opExtra()` is called zero or more times before and/or after the actual operation (the call to IRegisterTarget).
    This is used to pass along extra data.  
    For some examples (non exhaustive list):
     - `write()` does not use opExtra
     - `read()` calls opExtra once after the read is performed to report the value
     - `seqWrite()`, `fifoWrite()`, and `compWrite()` calls opExtra once for each data value before performing the write.
     - `seqRead()` and `fifoRead()` call opExtra once for each data value after the reads are performed
     - `compRead()` calls opExtra once for each address before the reads are performed and once for each data value after.
- Finally, either `opError()` or `opEnd()` is called at the end of the operation depending on whether there was an error or not.

The final use case for IFluentRegisterTargetInterposer is the ability to hold a "default" interposer that is used by all FluentRegisterTargets that don't explicitly specify one.  
`IFluentRegisterTargetInterposer::setDefault()` is used to set the default interposer.  
`IFluentRegisterTargetInterposer::getDefault()` returns a pointer to this interposer (or nullptr if one hasn't been set yet).

## CPoller
`CPoller` is a concept encompassing the algorithm for polling a register, used by `FluentRegisterTarget::pollRead()`.
Essentially, it's a function object that takes a callback as a parameter and returns a boolean indicating success/timeout.  
The callback passed into the poller is what acutally performs the register read and comparison:
```cpp
template <CPoller PollerType>
FluentRegisterTarget& FluentRegisterTarget::pollRead(PollerType poller, AddressType addr, DataType expected, DataType mask, std::string_view msg = "")
{
    ...
    bool const success = poller([&] {
        reg_val = this->target->read(addr) ;
        return (reg_val & mask) == expected_val;
    });
    // `success` indicates whether the polling operation succeeded (true) or timed out (false)
    ...
}
```

The Poller is what is responsible for *timing* of the polling operation.
See [BasicPoller](#basicpoller) for an example.

## BasicPoller
`BasicPoller` is a `CPoller` implementation with configurable timing.
It's constructor signature is:
```cpp
BasicPoller(std::chrono::microseconds initial_delay, std::chrono::microseconds wait_delay, std::chrono::microseconds timeout);
```

`initial_delay` is a delay that occurrs before any polling (register reads) occurr.   
`wait_delay` is a delay that occurrs after register reads if the value does not yet match the expected value.   
`timeout` is a duration after which the BasicPoller will exit and return false.
It does *not* include the `initial_delay`.

The CPoller concept is satisfied with this member function:
```cpp
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
```
