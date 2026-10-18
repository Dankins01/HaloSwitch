/*
WEAPON_TAGS.C

The Halo 2 weapons (the battle rifle, the SMG and Halo 2's shotgun), built
into tags when a survival arena's map has loaded (scenario_tags_load) and
added to its tag table, as the menus' are (menu_tags.c, which adds its own
after these). They come from h2weapons.pak in the data folder, which
tools/h2_weapons.py makes from the player's own copy of Halo 2: nothing of
Halo 2's is in this repository, and without the pack nothing is added.

Each weapon is a copy of a Halo weapon of the map's (its donor: the assault
rifle, the shotgun) with its own first- and third-person models, its
magazines and triggers as the pack sets them, and, for more or less damage,
its own copy of the donor's projectile and its damage effect. Everything
else (its animations, sounds, HUD and effects) is the donor's. A model is a
copy of the donor's (its nodes and markers, which the animations move) with
the pack's geometry: each part a texture of the pack's (a bitmap group of
one DXT texture), drawn with a copy of one of the donor's model shaders, its
vertices bound to the donor's nodes by name and placed in the donor's
space (the donor's gun node where Halo 2's is).

A weapon whose donor the map has not got is left out, and the shop
(survival.c) sells only those the map has. All of it is let go in
scenario_tags_unload, after the menus' tags.
*/

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "game/game_globals.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "models/model_definitions.h"
#include "objects/damage_effect_definitions.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_groups.h"

#include "survival.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
const char *platform_data_root(void);

/* cache_files.c's (port) */
void *cache_files_tag_instances(long *count);
void cache_files_set_tag_instances(void *instances, long count);

/* the game's */
char *tag_get_name(long tag_index);
boolean rasterizer_vertex_buffer_new(struct vertex_buffer *vertex_buffer, long vertex_type, long count,
	void const *vertices, long buffer_size);
void rasterizer_vertex_buffer_delete(struct vertex_buffer *vertex_buffer);
boolean rasterizer_triangle_buffer_new(struct triangle_buffer *triangle_buffer, short triangle_type, long count,
	void const *triangles);
void rasterizer_triangle_buffer_delete(struct triangle_buffer *triangle_buffer);

void weapon_tags_loaded(char const *map_name);
void weapon_tags_unloaded(void);

/* ---------- constants */

#define PACK_NAME "h2weapons.pak"
#define PACK_SIGNATURE "H2WP"
#define PACK_VERSION 2
#define PACK_TAG_NAME 64
#define PACK_DISPLAY_NAME 32
#define PACK_NODE_NAME 32
#define PACK_MAXIMUM_SIZE (64L * 1024 * 1024)
#define SHADER_PREFIX "h2weapons\\shaders\\"
#define TEXTURE_PREFIX "h2weapons\\"

#define MODEL_TAG 'mode'
#define SHADER_MODEL_TAG 'soso'
#define PROJECTILE_TAG 'proj'
#define DAMAGE_EFFECT_TAG 'jpt!'
#define WEAPON_TAG 'weap'

enum
{
	MAXIMUM_PACK_WEAPONS = 16,
	MAXIMUM_PACK_TEXTURES = 256,
	MAXIMUM_PACK_GEOMETRIES = 5,
	MAXIMUM_PART_INDICES = 1L << 20,
	NUMBER_OF_DETAIL_LEVELS = 5,
	/* the tags a weapon adds besides its shaders and textures: itself, its
	two models, a projectile and its damage effect */
	TAGS_PER_WEAPON = 5,
	/* a compressed model vertex's node index (a node's matrix's first of
	three constants), NONE's for a vertex of one node */
	COMPRESSED_NODE_SCALE = 3,
	COMPRESSED_NO_NODE = (byte)(NONE * COMPRESSED_NODE_SCALE),
};

/* a pack part's flags */
enum
{
	_pack_part_alpha_tested_bit = 0,
};

enum
{
	_bitmap_type_2d = 0,
	_bitmap_format_dxt1 = 14,
	_bitmap_format_dxt3 = 15,
	_bitmap_format_dxt5 = 16,
	_bitmap_has_power_of_two_dimensions_bit = 0,
	_bitmap_compressed_bit = 1,
};

enum
{
	_triangle_buffer_strips = 1,
};

enum
{
	_shader_model_two_sided_bit = 1,
	_shader_model_not_alpha_tested_bit = 2,
};

/* ---------- structures */

struct cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

typedef char verify_cache_file_tag_instance_size[
	sizeof(struct cache_file_tag_instance) == 0x20 ? 1 : -1];

/* a model's, as models.c has them */
struct model_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

struct model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

typedef char verify_model_shader_reference_size[sizeof(struct model_shader_reference) == 0x20 ? 1 : -1];
typedef char verify_model_geometry_size[sizeof(struct model_geometry) == 0x30 ? 1 : -1];
typedef char verify_model_geometry_part_size[sizeof(struct model_geometry_part) == 0x68 ? 1 : -1];
typedef char verify_model_vertex_compressed_size[sizeof(struct model_vertex_compressed) == 0x20 ? 1 : -1];

/* a model shader, as rasterizer_xbox_models.c has it */
struct weapon_shader_model
{
	struct shader shader;
	word flags;
	short type;
	byte reserved04[0xC];
	real translucency;
	byte reserved14[0x10];
	short change_color_source;
	byte reserved26[0x1E];
	word self_illumination_flags;
	short pad46;
	short self_illumination_color_source;
	short self_illumination_animation_function;
	real self_illumination_animation_period;
	real_rgb_color self_illumination_animation_color_lower_bound;
	real_rgb_color self_illumination_animation_color_upper_bound;
	byte reserved68[0xC];
	real map_u_scale;
	real map_v_scale;
	struct tag_reference base_map;
	byte reserved8C[8];
	struct tag_reference multipurpose_map;
	byte reservedA4[8];
	short detail_function;
	short detail_mask;
	real detail_map_scale;
	struct tag_reference detail_map;
	real detail_map_v_scale;
	byte reservedC8[0xC];
	struct shader_texture_animation texture_animation;
	byte reserved10C[8];
	real reflection_falloff_distance;
	real reflection_cutoff_distance;
	real perpendicular_brightness;
	real_rgb_color perpendicular_tint_color;
	real parallel_brightness;
	real_rgb_color parallel_tint_color;
	struct tag_reference reflection_cube_map;
};

typedef char verify_weapon_shader_model_flags[offsetof(struct weapon_shader_model, flags) == 0x28 ? 1 : -1];
typedef char verify_weapon_shader_model_change_color[
	offsetof(struct weapon_shader_model, change_color_source) == 0x4C ? 1 : -1];
typedef char verify_weapon_shader_model_illumination[
	offsetof(struct weapon_shader_model, self_illumination_animation_color_upper_bound) == 0x84 ? 1 : -1];
typedef char verify_weapon_shader_model_scale[offsetof(struct weapon_shader_model, map_u_scale) == 0x9C ? 1 : -1];
typedef char verify_weapon_shader_model_base_map[
	offsetof(struct weapon_shader_model, base_map.index) == 0xB0 ? 1 : -1];
typedef char verify_weapon_shader_model_multipurpose_map[
	offsetof(struct weapon_shader_model, multipurpose_map.index) == 0xC8 ? 1 : -1];
typedef char verify_weapon_shader_model_detail_map[
	offsetof(struct weapon_shader_model, detail_map.index) == 0xE8 ? 1 : -1];
typedef char verify_weapon_shader_model_reflection_cube_map[
	offsetof(struct weapon_shader_model, reflection_cube_map.index) == 0x170 ? 1 : -1];
typedef char verify_weapon_shader_model_size[sizeof(struct weapon_shader_model) == 0x174 ? 1 : -1];

/* the pack, as read (its data where it lies in the file's copy) */
struct pack_texture
{
	char name[PACK_TAG_NAME];
	short width;
	short height;
	short format;
	short levels;
	long size;
	byte const *data;
	/* its bitmap group's tag (NONE: none made) */
	long tag_index;
};

struct pack_part
{
	short texture;
	unsigned short flags;
	long vertex_count;
	long index_count;
	byte const *vertices;
	byte const *indices;
};

struct pack_geometry
{
	long part_count;
	struct pack_part *parts;
};

struct pack_model
{
	long node_count;
	char const *node_names;
	byte levels[NUMBER_OF_DETAIL_LEVELS];
	long geometry_count;
	struct pack_geometry geometries[MAXIMUM_PACK_GEOMETRIES];
};

struct pack_weapon
{
	char tag[PACK_TAG_NAME];
	char donor[PACK_TAG_NAME];
	char display[PACK_DISPLAY_NAME];
	short magazine;
	short maximum;
	real rounds_per_second;
	short projectiles_per_shot;
	short rounds_per_shot;
	real error;
	real zoom;
	real damage;
	struct pack_model first_person;
	struct pack_model third_person;
};

struct pack_reader
{
	byte const *cursor;
	byte const *end;
	boolean failed;
};

/* ---------- globals */

static struct
{
	/* what is let go when the map unloads */
	void **blocks;
	long block_count;
	struct bitmap_data **bitmaps;
	long bitmap_count;
	struct model_geometry_part **parts;
	long part_count;
	struct cache_file_tag_instance *original_instances;
	long original_count;
	boolean loaded;
	/* (said once: there is no pack) */
	boolean missing_logged;
} weapon_tags;

/* the build under way */
static struct
{
	struct cache_file_tag_instance *instances;
	long first_index;
	long first_salt;
	long next;
	long total;
	boolean failed;
	struct pack_texture *textures;
	long texture_count;
	/* the shaders made: each its texture, flags and the donor's it copies */
	struct
	{
		long texture;
		unsigned short flags;
		long source_index;
		long tag_index;
	} *shaders;
	long shader_count;
} build;

/* ---------- private code */

static void *allocate(long size)
{
	void *block = malloc(size > 0 ? size : 1);
	void **blocks;

	if (!block)
	{
		build.failed = TRUE;
		return NULL;
	}
	memset(block, 0, size > 0 ? size : 1);
	blocks = realloc(weapon_tags.blocks, (weapon_tags.block_count + 1) * sizeof(*weapon_tags.blocks));
	if (!blocks)
	{
		free(block);
		build.failed = TRUE;
		return NULL;
	}
	weapon_tags.blocks = blocks;
	weapon_tags.blocks[weapon_tags.block_count++] = block;
	return block;
}

static char *string_copy(char const *first, char const *second)
{
	char *copy = allocate((long)strlen(first) + (long)strlen(second) + 1);

	if (copy)
	{
		strcpy(copy, first);
		strcat(copy, second);
	}
	return copy ? copy : "";
}

/* ---------- the pack */

static void const *pack_take(struct pack_reader *reader, long size)
{
	byte const *data = reader->cursor;

	if (reader->failed || size < 0 || size > reader->end - reader->cursor)
	{
		reader->failed = TRUE;
		return NULL;
	}
	reader->cursor += size;
	return data;
}

static unsigned long pack_unsigned(struct pack_reader *reader, long size)
{
	byte const *data = pack_take(reader, size);
	unsigned long value = 0;
	long index;

	for (index = size - 1; data && index >= 0; index--)
		value = (value << 8) | data[index];
	return value;
}

static real pack_real(struct pack_reader *reader)
{
	unsigned long bits = pack_unsigned(reader, 4);
	real value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void pack_name(struct pack_reader *reader, char *name, long size)
{
	char const *data = pack_take(reader, size);

	memset(name, 0, size);
	if (data)
		memcpy(name, data, size - 1);
}

static void pack_model_read(struct pack_reader *reader, struct pack_model *model)
{
	long geometry_index, level;

	model->node_count = (long)pack_unsigned(reader, 2);
	model->node_names = pack_take(reader, model->node_count * PACK_NODE_NAME);
	for (level = 0; level < NUMBER_OF_DETAIL_LEVELS; level++)
		model->levels[level] = (byte)pack_unsigned(reader, 1);
	pack_take(reader, 1);
	model->geometry_count = (long)pack_unsigned(reader, 2);
	if (model->node_count < 1 || model->node_count > MAXIMUM_NODES_PER_MODEL || model->geometry_count < 1 ||
		model->geometry_count > MAXIMUM_PACK_GEOMETRIES)
	{
		reader->failed = TRUE;
	}
	for (level = 0; level < NUMBER_OF_DETAIL_LEVELS; level++)
	{
		if (model->levels[level] >= model->geometry_count)
			reader->failed = TRUE;
	}
	for (geometry_index = 0; !reader->failed && geometry_index < model->geometry_count; geometry_index++)
	{
		struct pack_geometry *geometry = &model->geometries[geometry_index];
		long part_index;

		geometry->part_count = (long)pack_unsigned(reader, 2);
		if (geometry->part_count > MAXIMUM_PARTS_PER_MODEL_GEOMETRY)
		{
			reader->failed = TRUE;
			break;
		}
		geometry->parts = allocate(geometry->part_count * sizeof(*geometry->parts));
		for (part_index = 0; geometry->parts && !reader->failed && part_index < geometry->part_count; part_index++)
		{
			struct pack_part *part = &geometry->parts[part_index];

			part->texture = (short)pack_unsigned(reader, 2);
			part->flags = (unsigned short)pack_unsigned(reader, 2);
			part->vertex_count = (long)pack_unsigned(reader, 4);
			part->index_count = (long)pack_unsigned(reader, 4);
			/* (a strip's indices are 16 bits; one triangle at least) */
			if (part->vertex_count < 3 || part->vertex_count > UNSIGNED_SHORT_MAX || part->index_count < 3 ||
				part->index_count > MAXIMUM_PART_INDICES)
			{
				reader->failed = TRUE;
				break;
			}
			part->vertices = pack_take(reader, part->vertex_count * (long)sizeof(struct model_vertex_compressed));
			part->indices = pack_take(reader, part->index_count * 2);
		}
		if (!geometry->parts)
			reader->failed = TRUE;
	}
}

/* the pack's textures and weapons; FALSE: none, or not one this reads */
static boolean pack_read(byte const *data, long size, struct pack_weapon **weapons, long *weapon_count)
{
	struct pack_reader reader;
	long version, index;

	reader.cursor = data;
	reader.end = data + size;
	reader.failed = FALSE;
	if (size < 16 || memcmp(data, PACK_SIGNATURE, 4))
		return FALSE;
	pack_take(&reader, 4);
	version = (long)pack_unsigned(&reader, 4);
	*weapon_count = (long)pack_unsigned(&reader, 4);
	build.texture_count = (long)pack_unsigned(&reader, 4);
	if (version != PACK_VERSION)
	{
		platform_log("h2 weapons: %s is version %ld; this build reads version %d (make it again with "
			"tools/h2_weapons.py)", PACK_NAME, version, PACK_VERSION);
		return FALSE;
	}
	if (*weapon_count < 0 || *weapon_count > MAXIMUM_PACK_WEAPONS || build.texture_count < 0 ||
		build.texture_count > MAXIMUM_PACK_TEXTURES)
	{
		return FALSE;
	}
	build.textures = allocate((build.texture_count + 1) * sizeof(*build.textures));
	*weapons = allocate((*weapon_count + 1) * sizeof(**weapons));
	if (!build.textures || !*weapons)
		return FALSE;
	for (index = 0; index < build.texture_count && !reader.failed; index++)
	{
		struct pack_texture *texture = &build.textures[index];

		pack_name(&reader, texture->name, sizeof(texture->name));
		texture->width = (short)pack_unsigned(&reader, 2);
		texture->height = (short)pack_unsigned(&reader, 2);
		texture->format = (short)pack_unsigned(&reader, 2);
		texture->levels = (short)pack_unsigned(&reader, 2);
		texture->size = (long)pack_unsigned(&reader, 4);
		texture->data = pack_take(&reader, texture->size);
		texture->tag_index = NONE;
	}
	for (index = 0; index < *weapon_count && !reader.failed; index++)
	{
		struct pack_weapon *weapon = &(*weapons)[index];

		pack_name(&reader, weapon->tag, sizeof(weapon->tag));
		pack_name(&reader, weapon->donor, sizeof(weapon->donor));
		pack_name(&reader, weapon->display, sizeof(weapon->display));
		weapon->magazine = (short)pack_unsigned(&reader, 2);
		weapon->maximum = (short)pack_unsigned(&reader, 2);
		weapon->rounds_per_second = pack_real(&reader);
		weapon->projectiles_per_shot = (short)pack_unsigned(&reader, 2);
		weapon->rounds_per_shot = (short)pack_unsigned(&reader, 2);
		weapon->error = pack_real(&reader);
		weapon->zoom = pack_real(&reader);
		weapon->damage = pack_real(&reader);
		pack_model_read(&reader, &weapon->first_person);
		pack_model_read(&reader, &weapon->third_person);
	}
	if (reader.failed || build.failed)
	{
		platform_log("h2 weapons: %s is damaged (make it again with tools/h2_weapons.py)", PACK_NAME);
		return FALSE;
	}
	return TRUE;
}

/* ---------- the tag table */

/* the tag table with count more entries, the existing ones as they are */
static struct cache_file_tag_instance *instances_grow(long count)
{
	long existing, index, salt = 0;
	struct cache_file_tag_instance *instances = cache_files_tag_instances(&existing);
	struct cache_file_tag_instance *grown;

	if (!instances)
		return NULL;
	if (existing < 0 || count < 0 || existing > UNSIGNED_SHORT_MAX - count)
	{
		platform_log("h2 weapons: %ld tags and the map's %ld are too many for a tag table", count, existing);
		return NULL;
	}
	grown = allocate((existing + count) * sizeof(*grown));
	if (!grown)
		return NULL;
	memcpy(grown, instances, existing * sizeof(*grown));
	for (index = 0; index < existing; index++)
	{
		long instance_salt = (unsigned long)instances[index].tag_index >> 16;

		if (instance_salt > salt)
			salt = instance_salt;
	}
	/* (nothing a name finds, until made) */
	for (index = existing; index < existing + count; index++)
	{
		grown[index].group_tag = NONE;
		grown[index].parent_group_tags[0] = NONE;
		grown[index].parent_group_tags[1] = NONE;
		grown[index].tag_index = NONE;
		grown[index].name = "";
	}
	weapon_tags.original_instances = instances;
	weapon_tags.original_count = existing;
	build.first_index = existing;
	build.first_salt = salt + 1;
	return grown;
}

/* a new tag, its data and name (with parents as the tag's it copies:
NONE, none) */
static long tag_add(long group_tag, long parents_of, char const *name, void *definition)
{
	struct cache_file_tag_instance *instance;
	long tag_index;

	if (build.failed || !definition || build.next >= build.total)
	{
		build.failed = TRUE;
		return NONE;
	}
	tag_index = ((build.first_salt + build.next) << 16) | (build.first_index + build.next);
	instance = &build.instances[build.first_index + build.next];
	build.next++;
	instance->group_tag = group_tag;
	instance->parent_group_tags[0] = NONE;
	instance->parent_group_tags[1] = NONE;
	if (parents_of != NONE)
	{
		struct cache_file_tag_instance const *source = &build.instances[DATUM_INDEX_TO_ABSOLUTE_INDEX(parents_of)];

		instance->parent_group_tags[0] = source->parent_group_tags[0];
		instance->parent_group_tags[1] = source->parent_group_tags[1];
	}
	instance->tag_index = tag_index;
	instance->name = string_copy(name, "");
	instance->base_address = definition;
	/* (found by name from now) */
	cache_files_set_tag_instances(build.instances, build.first_index + build.next);
	return tag_index;
}

static void reference_set(struct tag_reference *reference, long group_tag, long tag_index)
{
	reference->group_tag = group_tag;
	reference->index = tag_index;
	reference->name = tag_index == NONE ? "" : tag_get_name(tag_index);
	reference->name_length = (long)strlen(reference->name);
}

static long tag_group_of(long tag_index)
{
	return tag_index == NONE ? NONE : build.instances[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index)].group_tag;
}

/* ---------- textures */

static long texture_level_size(short format, long width, long height)
{
	long blocks = ((width + 3) / 4) * ((height + 3) / 4);

	return blocks * (format == _bitmap_format_dxt1 ? 8 : 16);
}

static long texture_build(struct pack_texture *texture)
{
	struct bitmap_group *group;
	struct bitmap_group_sequence *sequence;
	struct bitmap_data *bitmap;
	long tag_index, levels = 0, needed = 0, width = texture->width, height = texture->height;
	char name[PACK_TAG_NAME + 16];

	if ((texture->format != _bitmap_format_dxt1 && texture->format != _bitmap_format_dxt3 &&
		texture->format != _bitmap_format_dxt5) || width < 1 || height < 1 || width > 2048 || height > 2048 ||
		(width & (width - 1)) || (height & (height - 1)))
	{
		platform_log("h2 weapons: texture %s is not a power-of-two DXT texture", texture->name);
		return NONE;
	}
	/* (the levels the data has, each as the game finds it: bitmaps.c) */
	while (levels < texture->levels)
	{
		long size = texture_level_size(texture->format, width, height);

		if (needed + size > texture->size)
			break;
		needed += size;
		levels++;
		if (width == 1 && height == 1)
			break;
		width = MAX(1, width / 2);
		height = MAX(1, height / 2);
	}
	if (!levels)
	{
		platform_log("h2 weapons: texture %s has no data", texture->name);
		return NONE;
	}
	group = allocate(sizeof(*group));
	sequence = allocate(sizeof(*sequence));
	bitmap = allocate(sizeof(*bitmap));
	if (!group || !sequence || !bitmap)
		return NONE;
	sequence->first_bitmap_index = 0;
	sequence->bitmap_count = 1;
	group->format = texture->format == _bitmap_format_dxt1 ? 0 : texture->format == _bitmap_format_dxt3 ? 1 : 2;
	group->mipmap_count = (short)(levels - 1);
	group->sequences.count = 1;
	group->sequences.address = sequence;
	group->bitmaps.count = 1;
	group->bitmaps.address = bitmap;
	snprintf(name, sizeof(name), "%s%s", TEXTURE_PREFIX, texture->name);
	tag_index = tag_add(BITMAP_GROUP_TAG, NONE, name, group);
	if (tag_index == NONE)
		return NONE;
	bitmap->signature = BITMAP_GROUP_TAG;
	bitmap->width = texture->width;
	bitmap->height = texture->height;
	bitmap->depth = 1;
	bitmap->type = _bitmap_type_2d;
	bitmap->format = texture->format;
	bitmap->flags = FLAG(_bitmap_has_power_of_two_dimensions_bit) | FLAG(_bitmap_compressed_bit);
	bitmap->mipmap_count = (short)(levels - 1);
	bitmap->pixels_size = needed;
	bitmap->tag_index = tag_index;
	bitmap->cache_block_index = NONE;
	/* (its pixels, which rasterizer_bitmap_changed copies into its texture,
	stay in the pack's copy until the map unloads) */
	bitmap->base_address = (void *)texture->data;
	/* (none without a renderer: debug.null_renderer) */
	if (rasterizer_bitmap_new(bitmap))
	{
		struct bitmap_data **bitmaps = realloc(weapon_tags.bitmaps,
			(weapon_tags.bitmap_count + 1) * sizeof(*weapon_tags.bitmaps));

		if (bitmaps)
		{
			weapon_tags.bitmaps = bitmaps;
			weapon_tags.bitmaps[weapon_tags.bitmap_count++] = bitmap;
		}
		else
		{
			rasterizer_bitmap_delete(bitmap);
			build.failed = TRUE;
		}
		if (bitmap->hardware_format)
			rasterizer_bitmap_changed(bitmap);
	}
	return tag_index;
}

/* ---------- shaders */

/* the model's first model shader, which the weapon's are copies of */
static long shader_source(struct model const *model)
{
	long index;

	for (index = 0; index < model->shaders.count; index++)
	{
		struct model_shader_reference const *reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, index,
			struct model_shader_reference);

		if (reference->shader.index != NONE && tag_group_of(reference->shader.index) == SHADER_MODEL_TAG)
			return reference->shader.index;
	}
	return NONE;
}

/* a shader of the texture's: the source's, drawing the texture alone (no
detail, reflection, self-illumination nor change colour) */
static long shader_build(long texture, unsigned short flags, long source_index)
{
	struct weapon_shader_model *shader;
	char name[PACK_TAG_NAME + 64];
	long index, texture_tag;
	char const *short_name;

	for (index = 0; index < build.shader_count; index++)
	{
		if (build.shaders[index].texture == texture && build.shaders[index].flags == flags &&
			build.shaders[index].source_index == source_index)
		{
			return build.shaders[index].tag_index;
		}
	}
	texture_tag = VALID_INDEX(texture, build.texture_count) ? build.textures[texture].tag_index : NONE;
	shader = allocate(sizeof(*shader));
	if (!shader)
		return NONE;
	memcpy(shader, tag_get(SHADER_MODEL_TAG, source_index), sizeof(*shader));
	shader->flags &= ~FLAG(_shader_model_two_sided_bit);
	if (TEST_FLAG(flags, _pack_part_alpha_tested_bit))
		shader->flags = (shader->flags & ~FLAG(_shader_model_not_alpha_tested_bit)) | FLAG(_shader_model_two_sided_bit);
	else
		shader->flags |= FLAG(_shader_model_not_alpha_tested_bit);
	shader->translucency = 0.0f;
	shader->change_color_source = 0;
	shader->self_illumination_flags = 0;
	shader->self_illumination_color_source = 0;
	shader->self_illumination_animation_function = 0;
	memset(&shader->self_illumination_animation_color_lower_bound, 0,
		sizeof(shader->self_illumination_animation_color_lower_bound));
	memset(&shader->self_illumination_animation_color_upper_bound, 0,
		sizeof(shader->self_illumination_animation_color_upper_bound));
	shader->map_u_scale = 1.0f;
	shader->map_v_scale = 1.0f;
	memset(&shader->texture_animation, 0, sizeof(shader->texture_animation));
	reference_set(&shader->base_map, BITMAP_GROUP_TAG, texture_tag);
	reference_set(&shader->multipurpose_map, BITMAP_GROUP_TAG, NONE);
	reference_set(&shader->detail_map, BITMAP_GROUP_TAG, NONE);
	reference_set(&shader->reflection_cube_map, BITMAP_GROUP_TAG, NONE);
	short_name = VALID_INDEX(texture, build.texture_count) ? strrchr(build.textures[texture].name, '\\') : NULL;
	snprintf(name, sizeof(name), "%s%s%s %ld", SHADER_PREFIX, short_name ? short_name + 1 : "none",
		TEST_FLAG(flags, _pack_part_alpha_tested_bit) ? " alpha tested" : "", build.shader_count);
	index = tag_add(SHADER_MODEL_TAG, source_index, name, shader);
	if (index != NONE)
	{
		void *shaders = realloc(build.shaders, (build.shader_count + 1) * sizeof(*build.shaders));

		if (!shaders)
		{
			build.failed = TRUE;
			return NONE;
		}
		build.shaders = shaders;
		build.shaders[build.shader_count].texture = texture;
		build.shaders[build.shader_count].flags = flags;
		build.shaders[build.shader_count].source_index = source_index;
		build.shaders[build.shader_count].tag_index = index;
		build.shader_count++;
	}
	return index;
}

/* ---------- models */

static boolean part_build(struct model_geometry_part *part, struct pack_part const *source, short const *node_map,
	long node_count, real_point3d const *root)
{
	long vertex_size = (long)sizeof(struct model_vertex_compressed);
	struct model_vertex_compressed *vertices = allocate(source->vertex_count * vertex_size);
	unsigned short *indices = allocate(source->index_count * 2);
	struct model_geometry_part **parts;
	real_point3d sum;
	long index;

	sum.x = sum.y = sum.z = 0.0f;
	if (!vertices || !indices)
		return FALSE;
	memcpy(vertices, source->vertices, source->vertex_count * vertex_size);
	memcpy(indices, source->indices, source->index_count * 2);
	for (index = 0; index < source->index_count; index++)
	{
		if (indices[index] >= source->vertex_count)
			return FALSE;
	}
	for (index = 0; index < source->vertex_count; index++)
	{
		struct model_vertex_compressed *vertex = &vertices[index];
		long node = vertex->nodes[0] < node_count ? node_map[vertex->nodes[0]] : 0;

		/* (in the donor's space: its gun node where Halo 2's is) */
		vertex->position.x += root->x;
		vertex->position.y += root->y;
		vertex->position.z += root->z;
		vertex->nodes[0] = (byte)(node * COMPRESSED_NODE_SCALE);
		vertex->nodes[1] = COMPRESSED_NO_NODE;
		vertex->node_weight = SHORT_MAX;
		sum.x += vertex->position.x;
		sum.y += vertex->position.y;
		sum.z += vertex->position.z;
	}
	part->flags = 0;
	part->previous_part_index = NONE;
	part->next_part_index = NONE;
	part->centroid_primary_node_index = 0;
	part->centroid_secondary_node_index = 0;
	part->centroid_primary_node_weight = 1.0f;
	part->centroid_secondary_node_weight = 0.0f;
	part->centroid.x = sum.x / source->vertex_count;
	part->centroid.y = sum.y / source->vertex_count;
	part->centroid.z = sum.z / source->vertex_count;
	parts = realloc(weapon_tags.parts, (weapon_tags.part_count + 1) * sizeof(*weapon_tags.parts));
	if (!parts)
		return FALSE;
	weapon_tags.parts = parts;
	weapon_tags.parts[weapon_tags.part_count++] = part;
	/* (none without a renderer: debug.null_renderer) */
	if (rasterizer_vertex_buffer_new(&part->vertex_buffer, _rasterizer_vertex_type_model_compressed,
		source->vertex_count, vertices, source->vertex_count * vertex_size))
	{
		rasterizer_triangle_buffer_new(&part->triangle_buffer, _triangle_buffer_strips, source->index_count - 2,
			indices);
	}
	return TRUE;
}

/* a copy of the donor's model with the pack's geometry */
static long model_build(struct pack_model const *source, long donor_index, char const *name)
{
	struct model const *donor = model_definition_get(donor_index);
	struct model *model = allocate(sizeof(*model));
	struct model_region *region = allocate(sizeof(*region));
	struct model_region_permutation *permutation = allocate(sizeof(*permutation));
	struct model_geometry *geometries = allocate(source->geometry_count * sizeof(*geometries));
	struct model_shader_reference *shaders;
	short node_map[MAXIMUM_NODES_PER_MODEL];
	long shader_source_index = shader_source(donor);
	long part_total = 0, geometry_index, index, level;
	real_point3d root;

	if (!model || !region || !permutation || !geometries || build.failed)
		return NONE;
	if (shader_source_index == NONE || !donor->nodes.count || !donor->regions.count)
	{
		platform_log("h2 weapons: model %s has no model shader, nodes or regions to copy", tag_get_name(donor_index));
		return NONE;
	}
	for (geometry_index = 0; geometry_index < source->geometry_count; geometry_index++)
		part_total += source->geometries[geometry_index].part_count;
	shaders = allocate((part_total + 1) * sizeof(*shaders));
	if (!shaders)
		return NONE;
	memcpy(model, donor, sizeof(*model));
	/* (each of the pack's nodes the donor's of its name; else its root) */
	{
		struct model_node const *root_node = TAG_BLOCK_GET_ELEMENT(&donor->nodes, 0, struct model_node);

		root = root_node->default_translation;
	}
	for (index = 0; index < source->node_count; index++)
	{
		char const *node_name = source->node_names + index * PACK_NODE_NAME;
		long node_index;

		node_map[index] = 0;
		for (node_index = 0; node_index < donor->nodes.count; node_index++)
		{
			struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&donor->nodes, node_index, struct model_node);

			if (!strncmp(node->name, node_name, PACK_NODE_NAME))
			{
				node_map[index] = (short)node_index;
				break;
			}
		}
	}
	/* one region, its permutation (the donor's first's markers) drawing the
	pack's geometry for each level of detail */
	memcpy(region, TAG_BLOCK_GET_ELEMENT(&donor->regions, 0, struct model_region), sizeof(*region));
	{
		struct model_region const *donor_region = TAG_BLOCK_GET_ELEMENT(&donor->regions, 0, struct model_region);

		if (donor_region->permutations.count > 0)
		{
			memcpy(permutation, TAG_BLOCK_GET_ELEMENT(&donor_region->permutations, 0, struct model_region_permutation),
				sizeof(*permutation));
		}
	}
	for (level = 0; level < NUMBER_OF_DETAIL_LEVELS; level++)
		permutation->geometry_indices[level] = source->levels[level];
	region->permutations.count = 1;
	region->permutations.address = permutation;
	model->regions.count = 1;
	model->regions.address = region;
	model->shaders.count = 0;
	model->shaders.address = shaders;
	for (geometry_index = 0; geometry_index < source->geometry_count && !build.failed; geometry_index++)
	{
		struct pack_geometry const *pack_geometry = &source->geometries[geometry_index];
		struct model_geometry *geometry = &geometries[geometry_index];
		struct model_geometry_part *parts = allocate((pack_geometry->part_count + 1) * sizeof(*parts));

		if (!parts)
			return NONE;
		geometry->parts.count = pack_geometry->part_count;
		geometry->parts.address = parts;
		for (index = 0; index < pack_geometry->part_count; index++)
		{
			struct pack_part const *pack_part = &pack_geometry->parts[index];
			long shader_index = shader_build(pack_part->texture, pack_part->flags, shader_source_index);
			long reference_index;

			if (shader_index == NONE || !part_build(&parts[index], pack_part, node_map, source->node_count, &root))
			{
				build.failed = TRUE;
				return NONE;
			}
			for (reference_index = 0; reference_index < model->shaders.count; reference_index++)
			{
				if (shaders[reference_index].shader.index == shader_index)
					break;
			}
			if (reference_index == model->shaders.count)
			{
				reference_set(&shaders[reference_index].shader, SHADER_MODEL_TAG, shader_index);
				shaders[reference_index].permutation_index = 0;
				model->shaders.count++;
			}
			parts[index].shader_index = (short)reference_index;
		}
	}
	model->geometries.count = source->geometry_count;
	model->geometries.address = geometries;
	return tag_add(MODEL_TAG, donor_index, name, model);
}

/* ---------- weapons */

/* a copy of the trigger's projectile and its damage effect, its damage
scaled */
static long projectile_build(long projectile_index, real damage, char const *name)
{
	struct projectile_definition *projectile;
	struct damage_effect_definition *effect;
	long effect_index, index;

	if (projectile_index == NONE || tag_group_of(projectile_index) != PROJECTILE_TAG)
		return projectile_index;
	projectile = allocate(sizeof(*projectile));
	effect = allocate(sizeof(*effect));
	if (!projectile || !effect)
		return NONE;
	memcpy(projectile, tag_get(PROJECTILE_TAG, projectile_index), sizeof(*projectile));
	effect_index = projectile->projectile.impact_damage.index;
	if (effect_index == NONE || tag_group_of(effect_index) != DAMAGE_EFFECT_TAG)
		return projectile_index;
	memcpy(effect, tag_get(DAMAGE_EFFECT_TAG, effect_index), sizeof(*effect));
	effect->damage.damage_minimum *= damage;
	effect->damage.damage_lower_bound *= damage;
	effect->damage.damage_upper_bound *= damage;
	index = tag_add(DAMAGE_EFFECT_TAG, effect_index, string_copy(name, " damage"), effect);
	if (index == NONE)
		return NONE;
	reference_set(&projectile->projectile.impact_damage, DAMAGE_EFFECT_TAG, index);
	return tag_add(PROJECTILE_TAG, projectile_index, name, projectile);
}

static boolean weapon_build(struct pack_weapon const *source)
{
	long donor_index = tag_loaded(WEAPON_TAG, source->donor);
	struct weapon_definition const *donor;
	struct weapon_definition *weapon;
	struct weapon_magazine_definition *magazines;
	struct weapon_trigger_definition *triggers;
	long first_person_index, third_person_index, index;
	char name[PACK_TAG_NAME + 16];

	if (donor_index == NONE)
		return FALSE;
	donor = tag_get(WEAPON_TAG, donor_index);
	if (donor->object.model.index == NONE || donor->weapon.interface_definition.first_person_model.index == NONE ||
		tag_group_of(donor->object.model.index) != MODEL_TAG ||
		tag_group_of(donor->weapon.interface_definition.first_person_model.index) != MODEL_TAG)
	{
		platform_log("h2 weapons: %s has no models to copy", source->donor);
		return FALSE;
	}
	/* (from here, what fails is the build's: its tags are half made) */
	weapon = allocate(sizeof(*weapon));
	magazines = allocate((donor->weapon.magazines.count + 1) * sizeof(*magazines));
	triggers = allocate((donor->weapon.triggers.count + 1) * sizeof(*triggers));
	if (!weapon || !magazines || !triggers)
	{
		build.failed = TRUE;
		return FALSE;
	}
	memcpy(weapon, donor, sizeof(*weapon));
	snprintf(name, sizeof(name), "%s fp", source->tag);
	first_person_index = model_build(&source->first_person,
		donor->weapon.interface_definition.first_person_model.index, name);
	third_person_index = first_person_index == NONE ? NONE :
		model_build(&source->third_person, donor->object.model.index, source->tag);
	if (third_person_index == NONE)
	{
		build.failed = TRUE;
		return FALSE;
	}
	reference_set(&weapon->object.model, MODEL_TAG, third_person_index);
	reference_set(&weapon->weapon.interface_definition.first_person_model, MODEL_TAG, first_person_index);
	/* its magazines and triggers, as the pack has them (none: the donor's) */
	if (donor->weapon.magazines.count > 0)
		memcpy(magazines, donor->weapon.magazines.address, donor->weapon.magazines.count * sizeof(*magazines));
	if (donor->weapon.triggers.count > 0)
		memcpy(triggers, donor->weapon.triggers.address, donor->weapon.triggers.count * sizeof(*triggers));
	weapon->weapon.magazines.address = magazines;
	weapon->weapon.triggers.address = triggers;
	if (donor->weapon.magazines.count > 0 && source->magazine > 0 && source->maximum >= source->magazine)
	{
		magazines[0].rounds_loaded_maximum = source->magazine;
		magazines[0].rounds_total_maximum = source->maximum;
		magazines[0].rounds_total_initial = (short)MIN(source->maximum, source->magazine * 3);
	}
	for (index = 0; index < donor->weapon.triggers.count && index < 1; index++)
	{
		struct weapon_trigger_definition *trigger = &triggers[index];

		if (source->rounds_per_second > 0.0f)
		{
			trigger->initial_rate_of_fire = source->rounds_per_second;
			trigger->final_rate_of_fire = source->rounds_per_second;
		}
		if (source->projectiles_per_shot > 0)
			trigger->projectiles_per_shot = source->projectiles_per_shot;
		if (source->rounds_per_shot > 0)
			trigger->rounds_per_shot = source->rounds_per_shot;
		if (source->error > 0.0f)
		{
			trigger->projectile_error_angle_lower_bound *= source->error;
			trigger->projectile_error_angle_upper_bound *= source->error;
		}
		if (source->damage > 0.0f && source->damage != 1.0f)
		{
			long projectile_index = projectile_build(trigger->projectile.index, source->damage,
				string_copy(source->tag, " bullet"));

			if (projectile_index == NONE)
			{
				build.failed = TRUE;
				return FALSE;
			}
			reference_set(&trigger->projectile, PROJECTILE_TAG, projectile_index);
		}
	}
	if (source->zoom > 1.0f)
	{
		weapon->weapon.zoom_level_count = 1;
		weapon->weapon.zoom_magnification_minimum = source->zoom;
		weapon->weapon.zoom_magnification_maximum = source->zoom;
	}
	return tag_add(WEAPON_TAG, donor_index, source->tag, weapon) != NONE;
}

static void weapon_tags_release(void)
{
	long index;

	for (index = 0; index < weapon_tags.part_count; index++)
	{
		rasterizer_triangle_buffer_delete(&weapon_tags.parts[index]->triangle_buffer);
		rasterizer_vertex_buffer_delete(&weapon_tags.parts[index]->vertex_buffer);
	}
	for (index = 0; index < weapon_tags.bitmap_count; index++)
	{
		if (weapon_tags.bitmaps[index]->hardware_format)
			rasterizer_bitmap_delete(weapon_tags.bitmaps[index]);
	}
	if (weapon_tags.original_instances)
		cache_files_set_tag_instances(weapon_tags.original_instances, weapon_tags.original_count);
	/* (the game's free takes no NULL) */
	for (index = 0; index < weapon_tags.block_count; index++)
		free(weapon_tags.blocks[index]);
	if (weapon_tags.blocks)
		free(weapon_tags.blocks);
	if (weapon_tags.bitmaps)
		free(weapon_tags.bitmaps);
	if (weapon_tags.parts)
		free(weapon_tags.parts);
	{
		boolean missing_logged = weapon_tags.missing_logged;

		memset(&weapon_tags, 0, sizeof(weapon_tags));
		weapon_tags.missing_logged = missing_logged;
	}
}

/* ---------- public code */

void weapon_tags_loaded(
	char const *map_name)
{
	char path[1024];
	FILE *file;
	long size, weapon_count = 0, part_total = 0, index, made = 0;
	byte *data;
	struct pack_weapon *weapons = NULL;

	if (!survival_map_is_arena(map_name))
		return;
	/* (the game's paths are the data folder's: d:\\, as the Xbox's) */
	snprintf(path, sizeof(path), "%s/%s", platform_data_root(), PACK_NAME);
	file = fopen("d:\\" PACK_NAME, "rb");
	if (!file)
	{
		if (!weapon_tags.missing_logged)
			platform_log("h2 weapons: no %s (tools/h2_weapons.py makes it from Halo 2's maps)", path);
		weapon_tags.missing_logged = TRUE;
		return;
	}
	memset(&build, 0, sizeof(build));
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = size > 0 && size <= PACK_MAXIMUM_SIZE ? allocate(size) : NULL;
	if (!data || fread(data, 1, size, file) != (size_t)size)
	{
		fclose(file);
		platform_log("h2 weapons: could not read %s", path);
		goto failed;
	}
	fclose(file);
	if (!pack_read(data, size, &weapons, &weapon_count))
		goto failed;
	for (index = 0; index < weapon_count; index++)
	{
		long geometry_index;

		for (geometry_index = 0; geometry_index < weapons[index].first_person.geometry_count; geometry_index++)
			part_total += weapons[index].first_person.geometries[geometry_index].part_count;
		for (geometry_index = 0; geometry_index < weapons[index].third_person.geometry_count; geometry_index++)
			part_total += weapons[index].third_person.geometries[geometry_index].part_count;
	}
	/* (each texture a bitmap; each part a shader at most) */
	build.total = build.texture_count + weapon_count * TAGS_PER_WEAPON + part_total;
	build.instances = instances_grow(build.total);
	if (!build.instances)
		goto failed;
	for (index = 0; index < build.texture_count && !build.failed; index++)
		build.textures[index].tag_index = texture_build(&build.textures[index]);
	for (index = 0; index < weapon_count && !build.failed; index++)
	{
		if (tag_loaded(WEAPON_TAG, weapons[index].donor) == NONE)
			platform_log("h2 weapons: %s left out: the map has no %s", weapons[index].tag, weapons[index].donor);
		else if (weapon_build(&weapons[index]))
			made++;
	}
	if (build.failed)
		goto failed;
	weapon_tags.loaded = TRUE;
	platform_log("h2 weapons: %ld of %ld weapons added (%ld tags) to the map's %ld tags", made, weapon_count,
		build.next, build.first_index);
	goto done;

failed:
	platform_log("h2 weapons: none added");
	weapon_tags_release();

done:
	if (build.shaders)
		free(build.shaders);
	memset(&build, 0, sizeof(build));
}

void weapon_tags_unloaded(
	void)
{
	if (weapon_tags.loaded || weapon_tags.block_count)
		weapon_tags_release();
}
