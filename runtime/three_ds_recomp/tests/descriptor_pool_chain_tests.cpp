// Cadeia de pools de descriptor por quadro (fast/backends/descriptor_pool_chain.h).
// Bug: a janela no PC caía com vkAllocateDescriptorSets(native PICA) =
// VK_ERROR_OUT_OF_POOL_MEMORY quando o MK7 passava de 4096 sets num quadro do
// host (notebook lento, vários quadros do guest por quadro do host).
#include "fast/backends/descriptor_pool_chain.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct FakePool {
    int Id = -1;
};

// Pools falsos de capacidade fixa: o que o Vulkan faria com maxSets.
struct FakeDevice {
    size_t Capacity = 2U;
    std::vector<size_t> Used;
    size_t Created = 0U;
    size_t Destroyed = 0U;
    bool FailCreate = false;
    bool FailAllocate = false;

    bool Create(FakePool* pool) {
        if (FailCreate) return false;
        pool->Id = static_cast<int>(Used.size());
        Used.push_back(0U);
        ++Created;
        return true;
    }
    Fast::DescriptorPoolChain<FakePool>::Attempt Allocate(FakePool pool) {
        using Attempt = Fast::DescriptorPoolChain<FakePool>::Attempt;
        if (FailAllocate) return Attempt::Failed;
        auto& used = Used.at(static_cast<size_t>(pool.Id));
        if (used == Capacity) return Attempt::PoolFull;
        ++used;
        return Attempt::Allocated;
    }
};

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        FakeDevice device;
        Fast::DescriptorPoolChain<FakePool> chain;
        const auto create = [&](FakePool* pool) { return device.Create(pool); };
        const auto allocate = [&](FakePool pool) { return device.Allocate(pool); };
        for (int set = 0; set < 5; ++set) {
            Require(chain.Allocate(create, allocate),
                    "a set beyond the first pool's capacity was not allocated");
        }
        Require(chain.PoolCount() == 3U && device.Created == 3U,
                "5 sets in pools of 2 did not take exactly 3 pools");

        // Novo quadro: os pools voltam vazios e são reaproveitados.
        chain.Reset([&](FakePool pool) { device.Used.at(static_cast<size_t>(pool.Id)) = 0U; });
        for (int set = 0; set < 6; ++set) {
            Require(chain.Allocate(create, allocate), "a set after Reset was not allocated");
        }
        Require(chain.PoolCount() == 3U && device.Created == 3U,
                "the frame after Reset created pools instead of reusing them");
        Require(chain.Allocate(create, allocate) && chain.PoolCount() == 4U,
                "the chain did not grow again once the reused pools were full");

        // Erro que não é falta de espaço sobe sem criar pool.
        device.FailAllocate = true;
        Require(!chain.Allocate(create, allocate) && device.Created == 4U,
                "a failed allocation was retried in a new pool");
        device.FailAllocate = false;

        // Pool novo que já não comporta o set: falha, sem laço infinito.
        device.Capacity = 0U;
        chain.Reset([&](FakePool pool) { device.Used.at(static_cast<size_t>(pool.Id)) = 0U; });
        Require(!chain.Allocate(create, allocate), "a set that fits no pool was reported allocated");
        Require(device.Created == 5U, "a set that fits no pool kept creating pools");

        device.Capacity = 2U;
        device.FailCreate = true;
        Fast::DescriptorPoolChain<FakePool> empty;
        Require(!empty.Allocate(create, allocate), "a failed pool creation was reported allocated");

        chain.Destroy([&](FakePool) { ++device.Destroyed; });
        Require(device.Destroyed == 5U && chain.PoolCount() == 0U, "Destroy did not release every pool");
    } catch (const std::exception& error) {
        std::cerr << "descriptor_pool_chain_tests: " << error.what() << "\n";
        return 1;
    }
    std::cout << "descriptor_pool_chain_tests: ok\n";
    return 0;
}
