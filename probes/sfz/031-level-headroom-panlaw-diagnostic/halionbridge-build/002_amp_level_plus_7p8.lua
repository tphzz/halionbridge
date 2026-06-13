local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "002_amp_level_plus_7p8", "002 Amp Level +7.8", { ampLevel = 7.8 })
end
