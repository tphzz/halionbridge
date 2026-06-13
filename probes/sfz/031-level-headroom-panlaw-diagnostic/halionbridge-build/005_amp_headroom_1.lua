local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "005_amp_headroom_1", "005 Amp Headroom 1", { ampHeadroom = 1 })
end
