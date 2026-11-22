#pragma once
#include "StdInclude.hpp"
#include "Signatures.hpp"

namespace IWXMVM::T6
{
    using T6Addresses = Signatures::T6Addresses;

    // scans every signature the first time it's called (InitializeGameAddresses), which throws on one not found
    inline const T6Addresses& GetGameAddresses()
    {
        static T6Addresses addresses;
        return addresses;
    }
}  // namespace IWXMVM::T6
