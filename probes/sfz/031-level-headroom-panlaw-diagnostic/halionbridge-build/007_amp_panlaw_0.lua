local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "007_amp_panlaw_0", "007 Amp PanLaw 0", { ampPanLaw = 0 })
end
