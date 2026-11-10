#pragma once

#include <cstddef>
#include <vector>

namespace Fast {

// Pools de descriptor de um quadro do host que crescem sob demanda. O pool de
// tamanho fixo esgotava (VK_ERROR_OUT_OF_POOL_MEMORY) quando um quadro do host
// juntava muitos quadros do guest; o caminho NRI já cresce assim (29b8bdbb).
// Pool é o handle (VkDescriptorPool); as funções vêm de quem usa, para o teste
// rodar sem GPU.
template <typename Pool>
class DescriptorPoolChain {
  public:
    enum class Attempt { Allocated, PoolFull, Failed };

    // create(Pool*) -> bool; allocate(Pool) -> Attempt. Pool cheio passa para
    // o próximo (criando se preciso); um pool recém-criado cheio é falha, não laço.
    template <typename CreateFn, typename AllocateFn>
    bool Allocate(CreateFn&& create, AllocateFn&& allocate) {
        for (;;) {
            bool fresh = false;
            if (mActive == mPools.size()) {
                Pool pool{};
                if (!create(&pool)) return false;
                mPools.push_back(pool);
                fresh = true;
            }
            switch (allocate(mPools[mActive])) {
            case Attempt::Allocated:
                return true;
            case Attempt::Failed:
                return false;
            case Attempt::PoolFull:
                if (fresh) return false;
                ++mActive;
                break;
            }
        }
    }

    // Início do quadro, depois da fence: os sets antigos já não são usados.
    template <typename ResetFn>
    void Reset(ResetFn&& reset) {
        for (auto& pool : mPools) reset(pool);
        mActive = 0U;
    }

    template <typename DestroyFn>
    void Destroy(DestroyFn&& destroy) {
        for (auto& pool : mPools) destroy(pool);
        mPools.clear();
        mActive = 0U;
    }

    size_t PoolCount() const noexcept { return mPools.size(); }

  private:
    std::vector<Pool> mPools;
    size_t mActive = 0U;
};

} // namespace Fast
