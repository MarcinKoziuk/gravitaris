#pragma once

#include <gravitaris/game/game.hpp>

#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer.hpp>

namespace Gravitaris {

class CGame : public Game {
protected:
    SimpleModelRenderer m_simpleModelRenderer;
    ModelRenderer m_modelRenderer;

    std::unique_ptr<EntitySpawner> CreateEntitySpawner() override;
public:
    explicit CGame(IFilesystem& filesystem);

    void Render(double delta);
};

} // namespace Gravitaris
