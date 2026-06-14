local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "000_amp_pan_center", "000 Amp Pan Center", { ampPan = 0 })
end
