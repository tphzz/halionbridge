local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "009_layer_pan_left_100", "009 Layer Pan Left 100", { layerPan = -100 })
end
