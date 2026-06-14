local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "010_layer_pan_right_100", "010 Layer Pan Right 100", { layerPan = 100 })
end
