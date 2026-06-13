local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "003_layer_level_plus_7p8", "003 Layer Level +7.8", { layerLevel = 7.8 })
end
