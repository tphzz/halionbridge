local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "010_amp_panlaw_3", "010 Amp PanLaw 3", { ampPanLaw = 3 })
end
