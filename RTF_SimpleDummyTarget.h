// Copyright (c) 2024 Matt M Halenza
// SPDX-License-Identifier: MIT
#pragma once
#include "RTF.h"
#include <YALF/YALF.h>
#include <unordered_map>

namespace RTF {

template <typename AddressType, typename DataType>
class SimpleDummyRegisterTarget : public RTF::IRegisterTarget<AddressType, DataType>
{
public:
    SimpleDummyRegisterTarget(std::string_view name)
        : RTF::IRegisterTarget<AddressType, DataType>(name)
    {}
    virtual std::string_view getDomain() const override { return "SimpleDummyRegisterTarget"; }

    virtual void write(AddressType addr, DataType data) override
    {
        LOG_NOISE(this, "write(0x{:0{}x}, 0x{:0{}x})", addr, sizeof(AddressType) * 2, data, sizeof(DataType) * 2);
        this->regs[addr] = data;
    }
    virtual DataType read(AddressType addr) override
    {
        DataType const rv = this->regs[addr];
        LOG_NOISE(this, "read(0x{:0{}x}) -> 0x{:0{}x}", addr, sizeof(AddressType) * 2, rv, sizeof(DataType) * 2);
        return rv;
    }
protected:
    std::unordered_map<AddressType, DataType> regs;
};

}
