local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "004_amp_headroom_0", "004 Amp Headroom 0", { ampHeadroom = 0 })
end
