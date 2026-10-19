package dev.hl2craft.client;

import dev.hl2craft.HL2Craft;
import java.util.List;
import java.util.Optional;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.HolderSet;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.Difficulty;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.LevelSettings;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.FlatLevelSource;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.flat.FlatLayerInfo;
import net.minecraft.world.level.levelgen.flat.FlatLevelGeneratorSettings;
import net.minecraft.world.level.levelgen.presets.WorldPresets;

public final class MirrorWorld {
	private static boolean attempted;

	private MirrorWorld() {
	}

	public static void openWhenReady(Minecraft minecraft) {
		if (attempted || minecraft.level != null || minecraft.gui.overlay() != null) {
			return;
		}
		if (!(minecraft.gui.screen() instanceof TitleScreen)) {
			if (minecraft.gui.screen() == null || minecraft.gui.screen().getClass().getName().contains("Onboarding")) {
				minecraft.gui.setScreen(new TitleScreen());
			}
			return;
		}
		TitleScreen title = (TitleScreen) minecraft.gui.screen();
		attempted = true;

		HL2SetupManager.checkAndProceed(minecraft, () -> doOpen(minecraft, title));
	}

	private static void doOpen(Minecraft minecraft, TitleScreen title) {
		if (minecraft.getLevelSource().levelExists(HL2Craft.WORLD_NAME)) {
			HL2Craft.LOG.info("HL2Craft: loading existing mirror world");
			minecraft.createWorldOpenFlows().openWorld(HL2Craft.WORLD_NAME, () -> minecraft.gui.setScreen(title));
			return;
		}

		HL2Craft.LOG.info("HL2Craft: creating new mirror world");
		LevelSettings settings = new LevelSettings(
			HL2Craft.WORLD_NAME,
			GameType.SURVIVAL,
			new LevelSettings.DifficultySettings(Difficulty.NORMAL, false, false),
			true,
			WorldDataConfiguration.DEFAULT
		);
		minecraft.createWorldOpenFlows().createFreshLevel(
			HL2Craft.WORLD_NAME,
			settings,
			new WorldOptions(0L, false, false),
			MirrorWorld::voidWorld,
			title
		);
	}

	private static WorldDimensions voidWorld(final HolderLookup.Provider registries) {
		FlatLevelGeneratorSettings flat = new FlatLevelGeneratorSettings(
			Optional.of(HolderSet.direct()), registries.lookupOrThrow(Registries.BIOME).getOrThrow(Biomes.PLAINS), List.of()
		);
		flat.getLayersInfo().add(new FlatLayerInfo(1, Blocks.AIR));
		flat.updateLayers();
		return WorldPresets.createNormalWorldDimensions(registries).replaceOverworldGenerator(registries, new FlatLevelSource(flat));
	}
}
