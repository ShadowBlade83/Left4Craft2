package dev.hl2craft.client.render;

import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.hl2craft.HL2Craft;
import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import it.unimi.dsi.fastutil.longs.LongLinkedOpenHashSet;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.client.Minecraft;
import net.minecraft.client.model.geom.builders.UVPair;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.block.FluidRenderer;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.world.level.CardinalLighting;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.projectile.ItemSupplier;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.Arrow;
import net.minecraft.world.entity.projectile.arrow.SpectralArrow;
import net.minecraft.world.entity.projectile.arrow.ThrownTrident;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.ItemDisplayContext;
import net.minecraft.world.item.ItemStack;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.server.level.BlockDestructionProgress;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import java.util.ArrayList;
import java.util.List;

public final class WorldExporter {
	private static final int SECTIONS_PER_FRAME = 16;
	private static final double ENTITY_RANGE = 96.0;
	private static final LongLinkedOpenHashSet DIRTY = new LongLinkedOpenHashSet();
	private static final LongOpenHashSet SENT = new LongOpenHashSet();
	private static int sentGeneration = Integer.MIN_VALUE;
	private static ClientLevel sentLevel;
	private static SkyAtlas atlas;
	private static ModelBlockRenderer blockRenderer;
	private static FluidRenderer fluidRenderer;
	private static final MeshBuilder MESH = new MeshBuilder();
	private static final List<HL2Link.WorldEntity> ENTITIES = new ArrayList<>();
	private static final ItemStackRenderState ITEM_STATE = new ItemStackRenderState();
	private static final java.util.Map<Item, float[]> ICONS = new java.util.HashMap<>();
	private static final java.util.Map<BlockState, float[]> CUBE_FACES = new java.util.HashMap<>();
	private static final RandomSource RANDOM = RandomSource.create();

	private static int lastPlayerChunkX = Integer.MIN_VALUE;
	private static int lastPlayerChunkZ = Integer.MIN_VALUE;
	private static long lastScanTime = 0;

	private WorldExporter() {
	}

	public static void markDirty(int sx, int sy, int sz) {
		synchronized (DIRTY) {
			DIRTY.add(SectionPos.asLong(sx, sy, sz));
		}
	}

	public static void markDirtyNow(int sx, int sy, int sz) {
		synchronized (DIRTY) {
			DIRTY.addAndMoveToFirst(SectionPos.asLong(sx, sy, sz));
		}
	}

	private static long lastDiagMs;
	private static int writeFails;

	public static void frame(Minecraft minecraft, float partialTick) {
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null || !HL2Link.active()) {
			return;
		}
		if (sentGeneration != HL2Link.generation() || sentLevel != level || atlas == null || atlas.stale(minecraft)) {
			resendEverything(minecraft, level);
		}
		queueNearbyChunks(minecraft, level);
		meshDirtySections(level);
		long nowDiag = System.currentTimeMillis();
		if (nowDiag - lastDiagMs > 5000) {
			lastDiagMs = nowDiag;
			int dirty;
			synchronized (DIRTY) {
				dirty = DIRTY.size();
			}
			HL2Craft.LOG.info("Left4Craft: exporter dirty {} sent {} writeFails {} player chunk {},{} radius {}", dirty, SENT.size(), writeFails,
				SectionPos.blockToSectionCoord(minecraft.player.getBlockX()), SectionPos.blockToSectionCoord(minecraft.player.getBlockZ()),
				minecraft.options.getEffectiveRenderDistance());
		}
		exportEntities(minecraft, level, partialTick);
		AvatarExporter.frame(minecraft, atlas, partialTick);
		if (atlas != null) {
			atlas.animate(level.getGameTime(), region -> {
				ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
					.putInt(region.x()).putInt(region.y()).putInt(region.w()).putInt(region.h()).flip();
				return HL2Link.tryWriteRender(Proto.REN_ATLAS_REGION, header, region.pixels());
			});
		}
	}

	private static void queueNearbyChunks(Minecraft minecraft, ClientLevel level) {
		long now = System.currentTimeMillis();
		int pcx = SectionPos.blockToSectionCoord(minecraft.player.getBlockX());
		int pcz = SectionPos.blockToSectionCoord(minecraft.player.getBlockZ());
		if (pcx == lastPlayerChunkX && pcz == lastPlayerChunkZ && now - lastScanTime < 1000) {
			return;
		}
		lastPlayerChunkX = pcx;
		lastPlayerChunkZ = pcz;
		lastScanTime = now;

		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) continue;
				LevelChunkSection[] sections = chunk.getSections();
				for (int i = 0; i < sections.length; i++) {
					if (!sections[i].hasOnlyAir()) {
						long key = SectionPos.asLong(cx, chunk.getSectionYFromSectionIndex(i), cz);
						if (!SENT.contains(key)) {
							markDirty(cx, chunk.getSectionYFromSectionIndex(i), cz);
						}
					}
				}
			}
		}
	}

	private static void resendEverything(Minecraft minecraft, ClientLevel level) {
		sentLevel = level;
		AvatarExporter.reset();
		atlas = SkyAtlas.build(minecraft);
		boolean ao = minecraft.options.ambientOcclusion().get();
		blockRenderer = new ModelBlockRenderer(ao, true, minecraft.getBlockColors());
		fluidRenderer = new FluidRenderer(minecraft.getModelManager().getFluidStateModelSet());
		HL2Link.writeRender(Proto.REN_CLEAR_ALL, ByteBuffer.allocate(0), null);
		ByteBuffer header = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(atlas.width).putInt(atlas.height).flip();
		boolean ok = HL2Link.writeRender(Proto.REN_ATLAS, header, atlas.pixels.duplicate().clear());
		HL2Craft.LOG.info("HL2Craft: sent {}x{} texture atlas to HL2 ({})", atlas.width, atlas.height, ok ? "ok" : "FAILED");
		if (!ok) {
			sentGeneration = Integer.MIN_VALUE;
			return;
		}
		sentGeneration = HL2Link.generation();
		SENT.clear();
		lastPlayerChunkX = Integer.MIN_VALUE;
		lastPlayerChunkZ = Integer.MIN_VALUE;
		lastScanTime = 0;

		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = SectionPos.blockToSectionCoord(minecraft.player.getBlockX());
		int pcz = SectionPos.blockToSectionCoord(minecraft.player.getBlockZ());
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) continue;
				LevelChunkSection[] sections = chunk.getSections();
				for (int i = 0; i < sections.length; i++) {
					if (!sections[i].hasOnlyAir()) {
						markDirty(cx, chunk.getSectionYFromSectionIndex(i), cz);
					}
				}
			}
		}
	}

	private static void meshDirtySections(ClientLevel level) {
		long deadline = System.nanoTime() + 4_000_000L;
		int meshed = 0;
		while (meshed < SECTIONS_PER_FRAME && System.nanoTime() < deadline) {
			long key;
			synchronized (DIRTY) {
				if (DIRTY.isEmpty()) return;
				key = DIRTY.removeFirstLong();
			}
			if (meshSection(level, key)) {
				meshed++;
			}
		}
	}

	private static LevelChunkSection sectionAt(ClientLevel level, LevelChunk chunk, int sy) {
		int index = level.getSectionIndexFromSectionY(sy);
		return index >= 0 && index < chunk.getSections().length ? chunk.getSections()[index] : null;
	}

	private static boolean meshSection(ClientLevel level, long key) {
		int sx = SectionPos.x(key), sy = SectionPos.y(key), sz = SectionPos.z(key);
		LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
		if (chunk == null) {
			return false;
		}
		LevelChunkSection section = sectionAt(level, chunk, sy);
		boolean empty = section == null || section.hasOnlyAir();
		if (empty && !SENT.contains(key)) {
			return false;
		}
		MESH.reset();
		MESH.cardinal = level.cardinalLighting();
		if (!empty) {
			BlockPos origin = SectionPos.of(sx, sy, sz).origin();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			for (int y = 0; y < 16; y++) {
				for (int z = 0; z < 16; z++) {
					for (int x = 0; x < 16; x++) {
						pos.set(origin.getX() + x, origin.getY() + y, origin.getZ() + z);
						BlockState state = chunk.getBlockState(pos);
						if (state.isAir()) continue;

						FluidState fluid = state.getFluidState();
						if (!fluid.isEmpty()) {
							MESH.fluidBaseY = y;
							fluidRenderer.tesselate(level, pos, MESH, state, fluid);
						}
						if (state.getRenderShape() == RenderShape.MODEL) {
							var model = Minecraft.getInstance().getModelManager().getBlockStateModelSet().get(state);
							blockRenderer.tesselateBlock(MESH, x, y, z, level, pos.immutable(), state, model, state.getSeed(pos));
						}
					}
				}
			}
		}
		if (MESH.vertexCount() == 0 && !SENT.contains(key)) {
			return true;
		}
		ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(MESH.vertexCount()).flip();
		if (HL2Link.writeRender(Proto.REN_SECTION, header, MESH.bytes())) {
			if (MESH.vertexCount() > 0) {
				SENT.add(key);
				if (SENT.size() <= 10 || SENT.size() % 100 == 0) {
					HL2Craft.LOG.info("HL2Craft: sent section ({}, {}, {}) with {} vertices (total sent: {})", sx, sy, sz, MESH.vertexCount(), SENT.size());
				}
			} else {
				SENT.remove(key);
			}
			return true;
		} else {
			writeFails++;
			markDirty(sx, sy, sz);
			return false;
		}
	}

	private static void exportEntities(Minecraft minecraft, ClientLevel level, float partialTick) {
		ENTITIES.clear();
		Vec3 eye = minecraft.player.getEyePosition(partialTick);
		for (Entity e : level.entitiesForRendering()) {
			if (ENTITIES.size() >= Proto.MAX_WORLD_ENTITIES || e.distanceToSqr(eye) > ENTITY_RANGE * ENTITY_RANGE) {
				continue;
			}
			Vec3 p = e.getPosition(partialTick);
			if (e instanceof AbstractArrow arrow) {
				boolean trident = arrow instanceof ThrownTrident;
				int kind = trident ? Proto.WE_TRIDENT : Proto.WE_ARROW;
				float yaw = Mth.rotLerp(partialTick, arrow.yRotO, arrow.getYRot());
				float pitch = Mth.lerp(partialTick, arrow.xRotO, arrow.getXRot());
				float[] uv = trident ? ICONS.computeIfAbsent(Items.TRIDENT, i -> iconUv(minecraft, level, new ItemStack(i)))
					: atlas.arrowUv(arrow instanceof SpectralArrow ? 2 : arrow instanceof Arrow tippable && tippable.getColor() > 0 ? 1 : 0);
				if (uv != null) {
					ENTITIES.add(new HL2Link.WorldEntity(kind, e.getId(), (float) p.x, (float) p.y, (float) p.z, yaw, pitch, 1.0F, null, uv, 0));
				}
			} else if (e instanceof ItemEntity item) {
				float bob = Mth.sin((item.getAge() + partialTick) / 10.0F + item.bobOffs) * 0.1F + 0.1F;
				float spin = ItemEntity.getSpin(item.getAge() + partialTick, item.bobOffs) * Mth.RAD_TO_DEG;
				addItem(minecraft, level, e, item.getItem(), p.add(0, bob, 0), spin);
			} else if (e instanceof ItemSupplier supplier) {
				addItem(minecraft, level, e, supplier.getItem(), p.add(0, e.getBbHeight() * 0.5 - 0.25, 0), 0.0F);
			} else if (e instanceof net.minecraft.world.entity.item.PrimedTnt tnt) {
				BlockState state = tnt.getBlockState();
				float[] faces = cubeFaces(minecraft, state);
				if (faces != null) {
					float size = 0.98F;
					ENTITIES.add(new HL2Link.WorldEntity(
						Proto.WE_BLOCK, e.getId(), (float) p.x, (float) (p.y + size * 0.5), (float) p.z, 0.0F, 0.0F, size, null, faces, 0
					));
				}
			}
		}
		addCracks(level);
		HL2Link.writeWorldEntities(ENTITIES, selection(minecraft, level));
	}

	private static void addItem(Minecraft minecraft, ClientLevel level, Entity e, ItemStack stack, Vec3 p, float yaw) {
		if (stack.getItem() instanceof BlockItem blockItem) {
			BlockState state = blockItem.getBlock().defaultBlockState();
			if (state.getRenderShape() == RenderShape.MODEL
				&& Block.isShapeFullBlock(state.getShape(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO))) {
				float[] faces = cubeFaces(minecraft, state);
				if (faces != null) {
					float size = 0.25F;
					int tint = cubeTint(minecraft, state);
					ENTITIES.add(new HL2Link.WorldEntity(
						Proto.WE_BLOCK, e.getId(), (float) p.x, (float) (p.y + size * 0.5 + 0.02), (float) p.z, yaw, 0.0F, size, null, faces, tint
					));
					return;
				}
			}
		}
		float[] uv = iconUv(minecraft, level, stack);
		if (uv != null) {
			ENTITIES.add(new HL2Link.WorldEntity(Proto.WE_ITEM, e.getId(), (float) p.x, (float) p.y + 0.25F, (float) p.z, yaw, 0.0F, 0.5F, null, uv, 0));
		}
	}

	private static float[] cubeFaces(Minecraft minecraft, BlockState state) {
		return CUBE_FACES.computeIfAbsent(state, s -> {
			var model = minecraft.getModelManager().getBlockStateModelSet().get(s);
			var parts = new ArrayList<net.minecraft.client.renderer.block.dispatch.BlockStateModelPart>();
			RANDOM.setSeed(42);
			if (model != null) {
				model.collectParts(RANDOM, parts);
			}
			float[] out = new float[12];
			Direction[] dirs = { Direction.NORTH, Direction.UP, Direction.DOWN };
			for (int f = 0; f < 3; f++) {
				TextureAtlasSprite sprite = null;
				for (var part : parts) {
					var quads = part.getQuads(dirs[f]);
					if (!quads.isEmpty()) {
						sprite = quads.getFirst().materialInfo().sprite();
						break;
					}
				}
				if (sprite == null) {
					for (var part : parts) {
						var quads = part.getQuads(null);
						if (!quads.isEmpty()) {
							sprite = quads.getFirst().materialInfo().sprite();
							break;
						}
					}
				}
				if (sprite == null && !parts.isEmpty() && parts.getFirst().particleMaterial() != null) {
					sprite = parts.getFirst().particleMaterial().sprite();
				}
				if (sprite == null && model != null && model.particleMaterial() != null) {
					sprite = model.particleMaterial().sprite();
				}
				if (sprite == null) {
					var pm = minecraft.getModelManager().getBlockStateModelSet().getParticleMaterial(s);
					if (pm != null) {
						sprite = pm.sprite();
					}
				}
				if (sprite == null) {
					return null;
				}
				System.arraycopy(atlas.rect(sprite), 0, out, f * 4, 4);
			}
			return out;
		});
	}

	private static int cubeTint(Minecraft minecraft, BlockState state) {
		var sources = minecraft.getBlockColors().getTintSources(state);
		if (sources.isEmpty() || sources.getFirst() == null) {
			return 0;
		}
		int argb = sources.getFirst().color(state);
		return 0xFF000000 | (argb & 0xFF) << 16 | (argb & 0xFF00) | (argb >> 16 & 0xFF);
	}

	private static void addCracks(ClientLevel level) {
		for (BlockDestructionProgress progress : ((dev.hl2craft.client.mixin.ClientLevelAccessor) level).hl2craft$destroyingBlocks().values()) {
			int stage = progress.getProgress();
			if (stage < 0 || stage > 9 || ENTITIES.size() >= Proto.MAX_WORLD_ENTITIES) {
				continue;
			}
			BlockPos pos = progress.getPos();
			VoxelShape shape = level.getBlockState(pos).getShape(level, pos);
			if (shape.isEmpty()) {
				continue;
			}
			AABB box = shape.bounds().move(pos).inflate(0.004);
			ENTITIES.add(new HL2Link.WorldEntity(
				Proto.WE_CRACK, pos.hashCode(), (float) box.minX, (float) box.minY, (float) box.minZ, 0.0F, 0.0F, 1.0F,
				new float[] { (float) box.getXsize(), (float) box.getYsize(), (float) box.getZsize() }, atlas.crackUv(stage), 0
			));
		}
	}

	private static float[] iconUv(Minecraft minecraft, ClientLevel level, ItemStack stack) {
		minecraft.getItemModelResolver().updateForTopItem(ITEM_STATE, stack, ItemDisplayContext.GROUND, level, null, 0);
		RANDOM.setSeed(0);
		var material = ITEM_STATE.pickParticleMaterial(RANDOM);
		if (material == null) {
			return null;
		}
		return atlas.rect(material.sprite());
	}

	private static float[] selection(Minecraft minecraft, ClientLevel level) {
		HitResult hit = minecraft.hitResult;
		if (!(hit instanceof BlockHitResult blockHit) || hit.getType() != HitResult.Type.BLOCK || minecraft.gui.screen() != null) {
			return null;
		}
		if (blockHit instanceof dev.hl2craft.world.HL2Clip.HL2HitResult) {
			return null;
		}
		BlockPos pos = blockHit.getBlockPos();
		BlockState state = level.getBlockState(pos);
		if (state.isAir() || state.is(net.minecraft.world.level.block.Blocks.BARRIER)) {
			return null;
		}
		VoxelShape shape = state.getShape(level, pos);
		if (shape.isEmpty()) {
			return null;
		}
		AABB box = shape.bounds().move(pos);
		return new float[] { (float) box.minX, (float) box.minY, (float) box.minZ, (float) box.maxX, (float) box.maxY, (float) box.maxZ };
	}

	private static final class MeshBuilder implements net.minecraft.client.renderer.block.BlockQuadOutput, FluidRenderer.Output, VertexConsumer {
		private ByteBuffer buf = ByteBuffer.allocateDirect(1 << 20).order(ByteOrder.LITTLE_ENDIAN);
		private int vertices;
		private final float[] fq = new float[4 * 8];
		private int fqCount;
		private boolean fluidTranslucent;
		int fluidBaseY;
		CardinalLighting cardinal = CardinalLighting.DEFAULT;

		void reset() {
			this.buf.clear();
			this.vertices = 0;
			this.fqCount = 0;
		}

		int vertexCount() {
			return this.vertices;
		}

		ByteBuffer bytes() {
			return this.buf.duplicate().flip();
		}

		private void ensure(int bytes) {
			if (this.buf.remaining() < bytes) {
				ByteBuffer bigger = ByteBuffer.allocateDirect(Math.max(this.buf.capacity() * 2, this.buf.position() + bytes)).order(ByteOrder.LITTLE_ENDIAN);
				this.buf.flip();
				bigger.put(this.buf);
				this.buf = bigger;
			}
		}

		private void vertex(float x, float y, float z, float u, float v, int argb, int light, int flags) {
			this.buf.putFloat(x).putFloat(y).putFloat(z).putFloat(u).putFloat(v);
			this.buf.put((byte) (argb >> 16)).put((byte) (argb >> 8)).put((byte) argb).put((byte) (argb >>> 24));
			int block = (light >> 4) & 0xF;
			int sky = (light >> 20) & 0xF;
			this.buf.putInt(block | (sky << 8));
			this.buf.putInt(flags);
			this.vertices++;
		}

		private static int flags(boolean translucent, Direction normal) {
			return (translucent ? 2 : 1) | (normal == null ? 0 : (normal.ordinal() + 1) << 4);
		}

		private static int unshade(int argb, float shade) {
			if (shade >= 0.999F || shade <= 0.0F) return argb;
			int r = Math.min(255, Math.round(((argb >> 16) & 0xFF) / shade));
			int g = Math.min(255, Math.round(((argb >> 8) & 0xFF) / shade));
			int b = Math.min(255, Math.round((argb & 0xFF) / shade));
			return (argb & 0xFF000000) | (r << 16) | (g << 8) | b;
		}

		@Override
		public void put(float x, float y, float z, BakedQuad quad, com.mojang.blaze3d.vertex.QuadInstance instance) {
			this.ensure(6 * Proto.REN_VERTEX_BYTES);
			TextureAtlasSprite sprite = quad.materialInfo().sprite();
			boolean translucent = quad.materialInfo().layer().translucent();
			int emission = quad.materialInfo().lightEmission();
			Direction override = quad.materialInfo().shadeDirectionOverride();
			Direction normal = override == Direction.UP && quad.direction() != Direction.UP ? null : override != null ? override : quad.direction();
			float shade = this.cardinal.byFace(override != null ? override : quad.direction());
			int flags = flags(translucent, normal);
			for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
				var p = quad.position(k);
				long uv = quad.packedUV(k);
				float u = atlas.u(sprite, UVPair.unpackU(uv));
				float v = atlas.v(sprite, UVPair.unpackV(uv));
				this.vertex(p.x() + x, p.y() + y, p.z() + z, u, v, unshade(instance.getColor(k), shade), instance.getLightCoordsWithEmission(k, emission), flags);
			}
		}

		@Override
		public VertexConsumer getBuilder(ChunkSectionLayer layer) {
			this.fluidTranslucent = layer.translucent();
			return this;
		}

		@Override
		public void addVertex(float x, float y, float z, int color, float u, float v, int overlay, int light, float nx, float ny, float nz) {
			int o = this.fqCount * 8;
			this.fq[o] = x;
			this.fq[o + 1] = y;
			this.fq[o + 2] = z;
			this.fq[o + 3] = atlas.blockU(u);
			this.fq[o + 4] = atlas.blockV(v);
			this.fq[o + 5] = Float.intBitsToFloat(color);
			this.fq[o + 6] = Float.intBitsToFloat(light);
			if (++this.fqCount == 4) {
				this.fqCount = 0;
				this.ensure(6 * Proto.REN_VERTEX_BYTES);
				Direction normal = nx == 0 && ny == 0 && nz == 0 ? null : Direction.getApproximateNearest(nx, ny, nz);
				float shade = normal == null ? 1.0F
					: normal.getAxis() == Direction.Axis.Y ? this.cardinal.byFace(normal) : this.cardinal.up() * this.cardinal.byFace(normal);
				int flags = flags(this.fluidTranslucent, normal);
				for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
					int b = k * 8;
					this.vertex(this.fq[b], this.fq[b + 1], this.fq[b + 2], this.fq[b + 3], this.fq[b + 4],
						unshade(Float.floatToRawIntBits(this.fq[b + 5]), shade), Float.floatToRawIntBits(this.fq[b + 6]), flags);
				}
			}
		}

		@Override public VertexConsumer addVertex(float x, float y, float z) { throw new UnsupportedOperationException(); }
		@Override public VertexConsumer setColor(int r, int g, int b, int a) { return this; }
		@Override public VertexConsumer setColor(int color) { return this; }
		@Override public VertexConsumer setUv(float u, float v) { return this; }
		@Override public VertexConsumer setUv1(int u, int v) { return this; }
		@Override public VertexConsumer setUv2(int u, int v) { return this; }
		@Override public VertexConsumer setUv3(float u, float v) { return this; }
		@Override public VertexConsumer setNormal(float x, float y, float z) { return this; }
		@Override public VertexConsumer setLineWidth(float width) { return this; }
	}
}
