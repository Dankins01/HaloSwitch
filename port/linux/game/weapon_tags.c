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
vertices bound to the donor's nodes by name. In the first person they are
placed as Halo 2 has them about its root, the donor's gun node at rest in
its animations; in the third person as Halo 2 has them about the model's
origin, which a unit's hand holds.

The energy sword is built on the plasma pistol (held as it is): its
triggers strike at arm's length, unseen, with the Elites' sword's melee
damage, which its melee does too, and its blade glows with the Elites'
sword's shader, where the map has them.

A weapon whose donor the map has not got is left out, and the shop
(survival.c) sells only those the map has. All of it is let go in
scenario_tags_unload, after the menus' tags.
*/

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "game/game_globals.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "math/real_math.h"
#include "models/model_animation_definitions.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "objects/damage_effect_definitions.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_groups.h"
#include "game/game_allegiance.h"
#include "units/units.h"

#include "survival.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
void weapon_tags_burst_control(long weapon_index, word *control_flags);
boolean weapon_tags_first_person_view(long weapon_definition_index, real_point3d const *camera_position,
	real_vector3d const *camera_forward, real_vector3d const *camera_up, real_matrix4x3 *transform);
char const *config_string(char const *name);
boolean weapon_tags_rest_root(long weapon_definition_index, real_point3d *root);

/* ---------- constants */

#define PACK_NAME "h2weapons.pak"
#define PACK_SIGNATURE "H2WP"
#define PACK_VERSION 5
#define PACK_TAG_NAME 64
#define PACK_DISPLAY_NAME 32
#define PACK_NODE_NAME 32
/* a node's name and its Halo 2 bind pose (a matrix's forward, left, up and
position) */
#define PACK_NODE_SIZE (PACK_NODE_NAME + 12 * 4)
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
	/* the burst weapons fired at once (the players' and the AI's) */
	MAXIMUM_BURST_WEAPONS = 32,
	/* the pause between bursts (Halo 2's battle rifle's, 0.26 s) */
	BURST_PAUSE_TICKS = 8,
	NUMBER_OF_DETAIL_LEVELS = 5,
	/* the tags a weapon adds besides its shaders and textures: itself, its
	two models, a projectile and its damage effect for each of two
	triggers, and a melee's */
	TAGS_PER_WEAPON = 8,
	/* a compressed model vertex's node index (a node's matrix's first of
	three constants), NONE's for a vertex of one node */
	COMPRESSED_NODE_SCALE = 3,
	COMPRESSED_NO_NODE = (byte)(NONE * COMPRESSED_NODE_SCALE),
};

/* a pack part's flags */
enum
{
	_pack_part_alpha_tested_bit = 0,
	/* (a sword's blade: drawn with the Elites' sword's glow, if the map has
	it) */
	_pack_part_glow_bit,
	/* (the ammunition counter's digits: the assault rifle's counter's
	shader, counting the rounds loaded; ones then tens) */
	_pack_part_digit_ones_bit,
	_pack_part_digit_tens_bit,
};

/* the assault rifle's first-person counter (its digits a bitmap's frames,
told by the weapon's first function, its rounds loaded over its magazine's
size: as the Halo 2 weapon on its hold's) */
#define COUNTER_SHADER "weapons\\assault rifle\\fp\\shaders\\numbers"
#define COUNTER_SHADER_SIZE 0x64
#define COUNTER_SHADER_LIMIT_OFFSET 0x28

/* a pack weapon's kind */
enum
{
	_pack_weapon_gun = 0,
	/* its trigger a strike at arm's length, its melee the Elites' sword's */
	_pack_weapon_sword,
};

/* the Elites' sword's, which the sword uses where the map has them */
#define SWORD_GLOW_SHADER "weapons\\energy sword\\shaders\\es blade top"
#define SWORD_MELEE_DAMAGE "weapons\\energy sword\\melee"
#define SHADER_TRANSPARENT_CHICAGO_TAG 'schi'
/* the reach of the sword's strike, and its speed (world units) */
#define SWORD_STRIKE_RANGE 2.2f
#define SWORD_STRIKE_SPEED 60.0f

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
	byte const *nodes;
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
	short kind;
	real_vector3d first_person_offset;
	short burst;
	/* the first person moved (forward, left, up; yaw, pitch, roll) */
	real view[6];
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
	/* the weapons made, by definition: their bursts and first-person views */
	struct weapon_tags_special
	{
		long definition_index;
		short burst;
		short kind;
		real view[6];
		real_point3d pivot;
		char name[PACK_DISPLAY_NAME];
	} specials[MAXIMUM_PACK_WEAPONS];
	long special_count;
	/* each firing burst weapon's (object): its shots and its pause */
	struct weapon_tags_burst
	{
		long weapon_index;
		long last_fired;
		short shots;
		short pause;
		boolean firing;
	} bursts[MAXIMUM_BURST_WEAPONS];
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
	/* the weapon being made's magazine (its counter's highest) */
	short counter_limit;
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
	model->nodes = pack_take(reader, model->node_count * PACK_NODE_SIZE);
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
		weapon->kind = (short)pack_unsigned(&reader, 2);
		weapon->burst = (short)pack_unsigned(&reader, 2);
		weapon->first_person_offset.i = pack_real(&reader);
		weapon->first_person_offset.j = pack_real(&reader);
		weapon->first_person_offset.k = pack_real(&reader);
		{
			short view_index;

			for (view_index = 0; view_index < 6; view_index++)
				weapon->view[view_index] = pack_real(&reader);
		}
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

/* (the build's table while it is made, else the map's: after the build,
dual wielding's rest_pose asks too) */
static long tag_group_of(long tag_index)
{
	struct cache_file_tag_instance *instances = build.instances;
	long count = 0;

	if (tag_index == NONE)
		return NONE;
	if (!instances)
	{
		instances = cache_files_tag_instances(&count);
		if (!instances || DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) >= count)
			return NONE;
	}
	return instances[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index)].group_tag;
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

/* the counter's shader for a magazine of `limit`: the assault rifle's,
counting to it; NONE if the map has none */
static long counter_shader_build(short limit)
{
	long source = tag_loaded(SHADER_TRANSPARENT_CHICAGO_TAG, COUNTER_SHADER);
	char name[PACK_TAG_NAME + 64];
	byte *shader;
	long index;

	if (source == NONE || limit <= 0 || limit > 255)
		return NONE;
	snprintf(name, sizeof(name), "%scounter %d", SHADER_PREFIX, limit);
	index = tag_loaded(SHADER_TRANSPARENT_CHICAGO_TAG, name);
	if (index != NONE)
		return index;
	shader = allocate(COUNTER_SHADER_SIZE);
	if (!shader)
		return NONE;
	memcpy(shader, tag_get(SHADER_TRANSPARENT_CHICAGO_TAG, source), COUNTER_SHADER_SIZE);
	shader[COUNTER_SHADER_LIMIT_OFFSET] = (byte)limit;
	index = tag_add(SHADER_TRANSPARENT_CHICAGO_TAG, source, name, shader);
	platform_log("h2 weapons: an ammunition counter to %d%s", limit, index == NONE ? ": failed" : "");
	return index;
}

/* a shader of the texture's: the source's, drawing the texture alone (no
detail, reflection, self-illumination nor change colour) */
static long shader_build(long texture, unsigned short flags, long source_index)
{
	struct weapon_shader_model *shader;
	char name[PACK_TAG_NAME + 64];
	long index, texture_tag;
	char const *short_name;

	/* (a blade: the Elites' sword's own glow) */
	if (TEST_FLAG(flags, _pack_part_glow_bit))
	{
		long glow_index = tag_loaded(SHADER_TRANSPARENT_CHICAGO_TAG, SWORD_GLOW_SHADER);

		if (glow_index != NONE)
			return glow_index;
	}
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
	long node_count, real_matrix4x3 const *transforms)
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
		long pack_node = vertex->nodes[0] < node_count ? vertex->nodes[0] : 0;
		long node = node_map[pack_node];
		real_matrix4x3 const *transform = &transforms[pack_node];
		real_point3d position = vertex->position;
		real_vector3d vector, moved;

		/* (from its Halo 2 node's bind pose to its donor node's) */
		matrix4x3_transform_point(transform, &position, &vertex->position);
		vector = uncompress_int32_to_real_vector3d(vertex->normal);
		vertex->normal = compress_real_vector3d_to_int32_clamp(matrix4x3_transform_vector(transform, &vector, &moved));
		vector = uncompress_int32_to_real_vector3d(vertex->binormal);
		vertex->binormal = compress_real_vector3d_to_int32_clamp(matrix4x3_transform_vector(transform, &vector, &moved));
		vector = uncompress_int32_to_real_vector3d(vertex->tangent);
		vertex->tangent = compress_real_vector3d_to_int32_clamp(matrix4x3_transform_vector(transform, &vector, &moved));
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

/* the first-person animations' rest (the idle's first frame): each node's
matrix, by name; FALSE if it has none */
struct weapon_animation_graph_node
{
	char name[TAG_STRING_LENGTH+1];
	short next_sibling_node_index;
	short first_child_node_index;
	short parent_node_index;
	word pad;
	unsigned long flags;
	real_vector3d base_vector;
	real range;
	long pad1;
};

static boolean rest_pose(long animations_index, real_matrix4x3 *matrices, long *node_count)
{
	struct animation_graph const *graph;
	struct real_orientation orientations[MAXIMUM_NODES_PER_MODEL];
	long index;

	if (animations_index == NONE || tag_group_of(animations_index) != 'antr')
		return FALSE;
	graph = animation_graph_definition_get(animations_index);
	if (graph->nodes.count < 1 || graph->nodes.count > MAXIMUM_NODES_PER_MODEL)
		return FALSE;
	for (index = 0; index < graph->animations.count; index++)
	{
		struct animation const *animation = TAG_BLOCK_GET_ELEMENT(&graph->animations, index, struct animation);

		if (!strcmp(animation->name, "first-person idle") && animation->node_count == graph->nodes.count &&
			animation->frame_count > 0)
		{
			memset(orientations, 0, sizeof(orientations));
			animation_get_node_orientations(NULL, animation, 0, orientations);
			animation_graph_node_matrices_from_orientations(animations_index, matrices, orientations, global_origin3d,
				global_forward3d, global_up3d);
			*node_count = graph->nodes.count;
			return TRUE;
		}
	}
	return FALSE;
}

static long rest_pose_node(long animations_index, long node_count, char const *name)
{
	struct animation_graph const *graph = animation_graph_definition_get(animations_index);
	long index;

	for (index = 0; index < node_count; index++)
	{
		struct weapon_animation_graph_node const *node = TAG_BLOCK_GET_ELEMENT(&graph->nodes, index,
			struct weapon_animation_graph_node);

		if (!strncmp(node->name, name, TAG_STRING_LENGTH))
			return index;
	}
	return NONE;
}

/* a copy of the donor's model with the pack's geometry. Its vertices are
placed as Halo 2 has them about its root (the donor's gun's place): in the
first person as the animations' rest pose shows them, with each moving
part bound where its node is then; else as the donor's bind pose shows
them */
static long model_build(struct pack_model const *source, long donor_index, long animations_index,
	real_vector3d const *offset, real_point3d *root_at_rest, char const *name)
{
	struct model const *donor = model_definition_get(donor_index);
	struct model *model = allocate(sizeof(*model));
	struct model_region *region = allocate(sizeof(*region));
	struct model_region_permutation *permutation = allocate(sizeof(*permutation));
	struct model_geometry *geometries = allocate(source->geometry_count * sizeof(*geometries));
	struct model_shader_reference *shaders;
	short node_map[MAXIMUM_NODES_PER_MODEL];
	/* (the donor's nodes' bind poses, and what moves a vertex from each of
	the pack's nodes' to its donor node's) */
	real_matrix4x3 binds[MAXIMUM_NODES_PER_MODEL];
	real_matrix4x3 transforms[MAXIMUM_NODES_PER_MODEL];
	real_matrix4x3 pack_root_inverse;
	real_matrix4x3 rest[MAXIMUM_NODES_PER_MODEL];
	long rest_count = 0, rest_root = NONE;
	boolean has_rest;
	long shader_source_index = shader_source(donor);
	long part_total = 0, geometry_index, index, level;

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
	/* (each of the pack's nodes the donor's of its name, its vertices where
	that is; else the donor's root, as the pack's root is: the first) */
	model_get_node_matrices(donor, binds, NULL, NULL, NULL);
	has_rest = rest_pose(animations_index, rest, &rest_count);
	if (has_rest)
	{
		struct model_node const *root_node = TAG_BLOCK_GET_ELEMENT(&donor->nodes, 0, struct model_node);

		rest_root = rest_pose_node(animations_index, rest_count, root_node->name);
		has_rest = rest_root != NONE;
		if (has_rest && root_at_rest)
			*root_at_rest = rest[rest_root].position;
	}
	for (index = 0; index < source->node_count; index++)
	{
		byte const *record = source->nodes + index * PACK_NODE_SIZE;
		char const *node_name = (char const *)record;
		real_matrix4x3 pack_bind, inverse;
		real values[12];
		long node_index;

		memcpy(values, record + PACK_NODE_NAME, sizeof(values));
		pack_bind.scale = 1.0f;
		pack_bind.forward.i = values[0]; pack_bind.forward.j = values[1]; pack_bind.forward.k = values[2];
		pack_bind.left.i = values[3]; pack_bind.left.j = values[4]; pack_bind.left.k = values[5];
		pack_bind.up.i = values[6]; pack_bind.up.j = values[7]; pack_bind.up.k = values[8];
		pack_bind.position.x = values[9]; pack_bind.position.y = values[10]; pack_bind.position.z = values[11];
		matrix4x3_inverse(&pack_bind, &inverse);
		if (!index)
			pack_root_inverse = inverse;
		node_map[index] = NONE;
		for (node_index = 0; node_index < donor->nodes.count; node_index++)
		{
			struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&donor->nodes, node_index, struct model_node);

			if (!strncmp(node->name, node_name, PACK_NODE_NAME))
			{
				node_map[index] = (short)node_index;
				break;
			}
		}
		if (node_map[index] == NONE)
			node_map[index] = 0;
		/* (Halo 2's place about its root, then where the donor's node is
		at rest: bind * rest^-1 * rest_root * pack_root^-1) */
		{
			struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&donor->nodes, node_map[index], struct model_node);
			long rest_node = has_rest ? rest_pose_node(animations_index, rest_count, node->name) : NONE;
			real_matrix4x3 about_root, rest_inverse;

			if (rest_node != NONE)
			{
				real_matrix4x3 moved_root = pack_root_inverse;

				/* (moved in the root's space: the pack's first-person offset) */
				moved_root.position.x += offset->i;
				moved_root.position.y += offset->j;
				moved_root.position.z += offset->k;
				matrix4x3_multiply(&rest[rest_root], &moved_root, &about_root);
				matrix4x3_inverse(&rest[rest_node], &rest_inverse);
				matrix4x3_multiply(&rest_inverse, &about_root, &inverse);
				matrix4x3_multiply(&binds[node_map[index]], &inverse, &transforms[index]);
			}
			else
			{
				/* (the third person: as Halo 2 has it about the model's
				origin, which a unit's hand holds) */
				matrix4x3_identity(&transforms[index]);
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
			boolean digit = TEST_FLAG(pack_part->flags, _pack_part_digit_ones_bit) ||
				TEST_FLAG(pack_part->flags, _pack_part_digit_tens_bit);
			short permutation_index = TEST_FLAG(pack_part->flags, _pack_part_digit_tens_bit) ? 1 : 0;
			long shader_index = digit ? counter_shader_build(build.counter_limit) : NONE;
			long reference_index;

			/* (no counter: the digits drawn plain) */
			if (shader_index == NONE)
			{
				shader_index = shader_build(pack_part->texture,
					(unsigned short)(pack_part->flags & ~(FLAG(_pack_part_digit_ones_bit) | FLAG(_pack_part_digit_tens_bit))),
					shader_source_index);
				permutation_index = 0;
			}

			if (shader_index == NONE || !part_build(&parts[index], pack_part, node_map, source->node_count, transforms))
			{
				build.failed = TRUE;
				return NONE;
			}
			for (reference_index = 0; reference_index < model->shaders.count; reference_index++)
			{
				if (shaders[reference_index].shader.index == shader_index &&
					shaders[reference_index].permutation_index == permutation_index)
					break;
			}
			if (reference_index == model->shaders.count)
			{
				reference_set(&shaders[reference_index].shader, tag_group_of(shader_index), shader_index);
				shaders[reference_index].permutation_index = permutation_index;
				model->shaders.count++;
			}
			parts[index].shader_index = (short)reference_index;
		}
	}
	model->geometries.count = source->geometry_count;
	model->geometries.address = geometries;
	return tag_add(MODEL_TAG, donor_index, name, model);
}

/* ---------- bursts and views */

/* config.toml's h2weapons.view: "name=forward,left,up,yaw,pitch,roll;..."
(a weapon's tag's last name: "battle rifle", "smg"), for trying where a
weapon is held without remaking the pack */
static void weapon_view_override(
	struct weapon_tags_special *special)
{
	char const *text = config_string("h2weapons.view");
	size_t length = strlen(special->name);

	while (text && *text)
	{
		char const *end = strchr(text, ';');
		size_t piece = end ? (size_t)(end - text) : strlen(text);

		while (*text == ' ')
		{
			text++;
			piece--;
		}
		if (piece > length && !strncmp(text, special->name, length) && text[length] == '=')
		{
			real values[6];

			if (sscanf(text + length + 1, "%f,%f,%f,%f,%f,%f", &values[0], &values[1], &values[2], &values[3],
				&values[4], &values[5]) == 6)
			{
				memcpy(special->view, values, sizeof(values));
				platform_log("h2 weapons: %s held as h2weapons.view says", special->name);
			}
		}
		text = end ? end + 1 : NULL;
	}
}

static struct weapon_tags_special *weapon_special(
	long definition_index)
{
	long index;

	for (index = 0; index < weapon_tags.special_count; index++)
	{
		if (weapon_tags.specials[index].definition_index == definition_index)
			return &weapon_tags.specials[index];
	}
	return NULL;
}

/* ---------- weapons */

/* a copy of a damage effect, its damage scaled */
static long damage_effect_build(long effect_index, real damage, char const *name)
{
	struct damage_effect_definition *effect;

	if (effect_index == NONE || tag_group_of(effect_index) != DAMAGE_EFFECT_TAG)
		return effect_index;
	effect = allocate(sizeof(*effect));
	if (!effect)
		return NONE;
	memcpy(effect, tag_get(DAMAGE_EFFECT_TAG, effect_index), sizeof(*effect));
	effect->damage.damage_minimum *= damage;
	effect->damage.damage_lower_bound *= damage;
	effect->damage.damage_upper_bound *= damage;
	return tag_add(DAMAGE_EFFECT_TAG, effect_index, name, effect);
}

/* a copy of the trigger's projectile: its damage effect a copy scaled, or
the one given; a strike's unseen, gone at arm's length */
static long projectile_build(long projectile_index, real damage, long effect_index, boolean strike, char const *name)
{
	struct projectile_definition *projectile;

	if (projectile_index == NONE || tag_group_of(projectile_index) != PROJECTILE_TAG)
		return projectile_index;
	projectile = allocate(sizeof(*projectile));
	if (!projectile)
		return NONE;
	memcpy(projectile, tag_get(PROJECTILE_TAG, projectile_index), sizeof(*projectile));
	if (effect_index == NONE)
	{
		effect_index = damage_effect_build(projectile->projectile.impact_damage.index, damage,
			string_copy(name, " damage"));
		if (effect_index == NONE && projectile->projectile.impact_damage.index != NONE)
			return NONE;
	}
	reference_set(&projectile->projectile.impact_damage, DAMAGE_EFFECT_TAG, effect_index);
	if (strike)
	{
		reference_set(&projectile->object.model, MODEL_TAG, NONE);
		reference_set(&projectile->projectile.effect, projectile->projectile.effect.group_tag, NONE);
		projectile->projectile.maximum_range = SWORD_STRIKE_RANGE;
		projectile->projectile.initial_velocity = SWORD_STRIKE_SPEED / TICKS_PER_SECOND;
		projectile->projectile.final_velocity = SWORD_STRIKE_SPEED / TICKS_PER_SECOND;
		projectile->projectile.air_gravity_scale = 0.0f;
	}
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
	real_point3d root_at_rest;

	root_at_rest.x = root_at_rest.y = root_at_rest.z = 0.0f;

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
	build.counter_limit = source->magazine > 0 ? source->magazine :
		donor->weapon.magazines.count > 0 ?
			((struct weapon_magazine_definition *)donor->weapon.magazines.address)[0].rounds_loaded_maximum : 0;
	first_person_index = model_build(&source->first_person,
		donor->weapon.interface_definition.first_person_model.index,
		donor->weapon.interface_definition.first_person_animations.index, &source->first_person_offset, &root_at_rest,
		name);
	third_person_index = first_person_index == NONE ? NONE :
		model_build(&source->third_person, donor->object.model.index, NONE, &source->first_person_offset, NULL,
			source->tag);
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
	/* (a sword's triggers each its strike; a gun's first as the pack has
	it) */
	for (index = 0; index < donor->weapon.triggers.count && (index < 1 || source->kind == _pack_weapon_sword); index++)
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
		if (source->kind == _pack_weapon_sword)
		{
			/* (the strike: the sword's melee damage, else four times the
			donor's projectile's) */
			long melee_index = tag_loaded(DAMAGE_EFFECT_TAG, SWORD_MELEE_DAMAGE);
			long projectile_index = projectile_build(trigger->projectile.index, melee_index == NONE ? 4.0f : 1.0f,
				melee_index, TRUE, string_copy(source->tag, " strike"));

			if (projectile_index == NONE)
			{
				build.failed = TRUE;
				return FALSE;
			}
			reference_set(&trigger->projectile, PROJECTILE_TAG, projectile_index);
			/* (swung more often than the pistol's shots before it is too hot) */
			trigger->heat_generated_per_round *= 0.35f;
		}
		else if (source->damage > 0.0f && source->damage != 1.0f)
		{
			long projectile_index = projectile_build(trigger->projectile.index, source->damage, NONE, FALSE,
				string_copy(source->tag, " bullet"));

			if (projectile_index == NONE)
			{
				build.failed = TRUE;
				return FALSE;
			}
			reference_set(&trigger->projectile, PROJECTILE_TAG, projectile_index);
		}
	}
	if (source->kind == _pack_weapon_sword)
	{
		/* (the sword's melee where the map has it, else four times the
		donor's) */
		long melee_index = tag_loaded(DAMAGE_EFFECT_TAG, SWORD_MELEE_DAMAGE);

		if (melee_index == NONE)
		{
			melee_index = damage_effect_build(donor->weapon.melee_attack_damage.index, 4.0f,
				string_copy(source->tag, " melee"));
			if (melee_index == NONE && donor->weapon.melee_attack_damage.index != NONE)
			{
				build.failed = TRUE;
				return FALSE;
			}
		}
		reference_set(&weapon->weapon.melee_attack_damage, DAMAGE_EFFECT_TAG, melee_index);
		weapon->weapon.zoom_level_count = 0;
	}
	if (source->zoom > 1.0f)
	{
		weapon->weapon.zoom_level_count = 1;
		weapon->weapon.zoom_magnification_minimum = source->zoom;
		weapon->weapon.zoom_magnification_maximum = source->zoom;
	}
	index = tag_add(WEAPON_TAG, donor_index, source->tag, weapon);
	if (index == NONE)
		return FALSE;
	/* (its burst and first-person view, which the weapon and first-person
	code ask for) */
	if (weapon_tags.special_count < MAXIMUM_PACK_WEAPONS)
	{
		struct weapon_tags_special *special = &weapon_tags.specials[weapon_tags.special_count++];
		char const *short_name = strrchr(source->tag, '\\');

		special->definition_index = index;
		special->burst = source->burst;
		special->kind = source->kind;
		memcpy(special->view, source->view, sizeof(special->view));
		special->pivot = root_at_rest;
		snprintf(special->name, sizeof(special->name), "%s", short_name ? short_name + 1 : source->tag);
		weapon_view_override(special);
	}
	return TRUE;
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

static struct
{
	long definition_index;
	boolean found;
	real_point3d root;
} rest_roots[32];
static short rest_root_count;

void weapon_tags_loaded(
	char const *map_name)
{
	char path[1024];
	FILE *file;
	long size, weapon_count = 0, part_total = 0, index, made = 0;
	byte *data;
	struct pack_weapon *weapons = NULL;

	rest_root_count = 0;
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
		boolean later = FALSE;
		long other;

		/* (a weapon's first record its map has the donor of; a later one, a
		fallback, only if none before it was built) */
		if (tag_loaded(WEAPON_TAG, weapons[index].tag) != NONE)
			continue;
		for (other = index + 1; other < weapon_count; other++)
			later |= !strcmp(weapons[other].tag, weapons[index].tag);
		if (tag_loaded(WEAPON_TAG, weapons[index].donor) == NONE)
		{
			if (!later)
				platform_log("h2 weapons: %s left out: the map has none of its donors", weapons[index].tag);
		}
		else if (weapon_build(&weapons[index]))
		{
			made++;
		}
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
	rest_root_count = 0;
	if (weapon_tags.loaded || weapon_tags.block_count)
		weapon_tags_release();
}

/* a burst weapon's trigger (weapons.c's weapon_update, before the triggers
are looked at): pulled, it fires its burst's shots, then lets go for a
pause; held, it fires the next burst after it */
void weapon_tags_burst_control(
	long weapon_index,
	word *control_flags)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_tags_special *special = weapon_special(weapon->definition_index);
	boolean pulled = TEST_FLAG(*control_flags, _weapon_control_primary_trigger_bit);
	long index, free_index = NONE;

	if (!special || special->burst <= 0)
		return;
	for (index = 0; index < MAXIMUM_BURST_WEAPONS; index++)
	{
		if (weapon_tags.bursts[index].weapon_index == weapon_index)
			break;
		if (free_index == NONE && (weapon_tags.bursts[index].weapon_index == 0 ||
			weapon_tags.bursts[index].weapon_index == NONE ||
			!object_try_and_get_and_verify_type(weapon_tags.bursts[index].weapon_index, _object_mask_weapon)))
		{
			free_index = index;
		}
	}
	if (index == MAXIMUM_BURST_WEAPONS)
	{
		if (free_index == NONE || !pulled)
			return;
		index = free_index;
		memset(&weapon_tags.bursts[index], 0, sizeof(weapon_tags.bursts[index]));
		weapon_tags.bursts[index].weapon_index = weapon_index;
		weapon_tags.bursts[index].last_fired = weapon->weapon.game_time_last_fired;
	}
	{
		struct weapon_tags_burst *burst = &weapon_tags.bursts[index];

		if (weapon->weapon.game_time_last_fired != burst->last_fired)
		{
			burst->last_fired = weapon->weapon.game_time_last_fired;
			burst->shots++;
		}
		if (burst->firing && (burst->shots >= special->burst || weapon->weapon.magazines[0].rounds_loaded <= 0))
		{
			burst->firing = FALSE;
			burst->pause = BURST_PAUSE_TICKS;
		}
		if (!burst->firing)
		{
			if (burst->pause > 0)
				burst->pause--;
			else if (pulled)
			{
				burst->firing = TRUE;
				burst->shots = 0;
			}
		}
		SET_FLAG(*control_flags, _weapon_control_primary_trigger_bit, burst->firing);
	}
}

/* the first person of a weapon held where Halo 2 holds it: the move of
the arms and weapon, in the world, about the weapon's root (first_person_
weapons.c); FALSE for any other */
boolean weapon_tags_first_person_view(
	long weapon_definition_index,
	real_point3d const *camera_position,
	real_vector3d const *camera_forward,
	real_vector3d const *camera_up,
	real_matrix4x3 *transform)
{
	struct weapon_tags_special *special = weapon_special(weapon_definition_index);
	real_matrix4x3 camera, camera_inverse, view, about, moved, rotation;
	long index;
	boolean any = FALSE;

	if (!special)
		return FALSE;
	for (index = 0; index < 6; index++)
		any |= special->view[index] != 0.0f;
	if (!any)
		return FALSE;
	/* (in the camera's space: forward x, left y, up z) */
	matrix4x3_from_point_and_vectors(&camera, camera_position, camera_forward, camera_up);
	matrix4x3_inverse(&camera, &camera_inverse);
	/* yaw about up, then pitch about left (forward down), then roll about
	forward */
	{
		real yaw = special->view[3] * (real)M_PI / 180.0f, pitch = special->view[4] * (real)M_PI / 180.0f,
			roll = special->view[5] * (real)M_PI / 180.0f;
		real cy = (real)cos(yaw), sy = (real)sin(yaw), cp = (real)cos(pitch), sp = (real)sin(pitch),
			cr = (real)cos(roll), sr = (real)sin(roll);
		/* R = Rz(yaw) Ry(pitch) Rx(roll), columns: where forward, left and up go */
		real r[3][3];

		r[0][0] = cy * cp; r[0][1] = cy * sp * sr - sy * cr; r[0][2] = cy * sp * cr + sy * sr;
		r[1][0] = sy * cp; r[1][1] = sy * sp * sr + cy * cr; r[1][2] = sy * sp * cr - cy * sr;
		r[2][0] = -sp; r[2][1] = cp * sr; r[2][2] = cp * cr;
		matrix4x3_identity(&rotation);
		rotation.forward.i = r[0][0]; rotation.forward.j = r[1][0]; rotation.forward.k = r[2][0];
		rotation.left.i = r[0][1]; rotation.left.j = r[1][1]; rotation.left.k = r[2][1];
		rotation.up.i = r[0][2]; rotation.up.j = r[1][2]; rotation.up.k = r[2][2];
	}
	/* (about the root: moved to the origin, turned, moved back and on) */
	matrix4x3_identity(&about);
	about.position.x = -special->pivot.x;
	about.position.y = -special->pivot.y;
	about.position.z = -special->pivot.z;
	matrix4x3_multiply(&rotation, &about, &moved);
	moved.position.x += special->pivot.x + special->view[0];
	moved.position.y += special->pivot.y + special->view[1];
	moved.position.z += special->pivot.z + special->view[2];
	matrix4x3_multiply(&moved, &camera_inverse, &view);
	matrix4x3_multiply(&camera, &view, transform);
	return TRUE;
}

/* dual wielding's (first_person_weapons.c): where a weapon's first-person
model's root is in its idle animation's first frame, the camera's space
(forward, left, up); FALSE if it has none. Remembered for the map. */
boolean weapon_tags_rest_root(
	long weapon_definition_index,
	real_point3d *root)
{
	struct weapon_definition *definition;
	real_matrix4x3 rest[MAXIMUM_NODES_PER_MODEL];
	long rest_count = 0, model_index, animations_index, node;
	short index;

	for (index = 0; index < rest_root_count; index++)
	{
		if (rest_roots[index].definition_index == weapon_definition_index)
		{
			*root = rest_roots[index].root;
			return rest_roots[index].found;
		}
	}
	definition = weapon_definition_get(weapon_definition_index);
	model_index = definition->weapon.interface_definition.first_person_model.index;
	animations_index = definition->weapon.interface_definition.first_person_animations.index;
	root->x = root->y = root->z = 0.0f;
	if (rest_root_count < (short)NUMBEROF(rest_roots))
	{
		boolean found = FALSE;

		if (model_index != NONE && rest_pose(animations_index, rest, &rest_count))
		{
			struct model *model = model_definition_get(model_index);

			if (model->nodes.count > 0)
			{
				struct model_node *model_root = TAG_BLOCK_GET_ELEMENT(&model->nodes, 0, struct model_node);

				node = rest_pose_node(animations_index, rest_count, model_root->name);
				if (node != NONE)
				{
					*root = rest[node].position;
					found = TRUE;
				}
			}
		}
		rest_roots[rest_root_count].definition_index = weapon_definition_index;
		rest_roots[rest_root_count].found = found;
		rest_roots[rest_root_count].root = *root;
		rest_root_count++;
		return found;
	}
	return FALSE;
}

/* ---------- the sword */

/* how far and how wide (the aim's cosine) a sword's lunge reaches, and how
fast it carries the wielder (world units, per tick) */
#define SWORD_LUNGE_RANGE 5.0f
#define SWORD_LUNGE_COSINE 0.85f
#define SWORD_LUNGE_SPEED 0.32f

/* player_control.c, a unit's controls this tick: with the sword in hand the
trigger swings it, as Halo 2's (the melee, the sword's own, its swing the
hold's), and a swing begun with an enemy near in the aim lunges at it */
void weapon_tags_sword_input(
	long unit_index,
	unsigned long *unit_control_flags)
{
	static long swinging_units[4] = { NONE, NONE, NONE, NONE };
	struct unit_datum *unit = unit_index != NONE ? object_try_and_get_and_verify_type(unit_index, _object_mask_unit) : NULL;
	long weapon_index;
	struct weapon_datum *weapon;
	boolean sword = FALSE, pressed;
	short slot = (short)(DATUM_INDEX_TO_ABSOLUTE_INDEX(unit_index) & 3);
	long index;

	if (!unit || !VALID_INDEX(unit->unit.current_weapon_index, MAXIMUM_WEAPONS_PER_UNIT))
		return;
	weapon_index = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
	weapon = weapon_index != NONE ? object_try_and_get_and_verify_type(weapon_index, _object_mask_weapon) : NULL;
	for (index = 0; weapon && index < weapon_tags.special_count; index++)
	{
		if (weapon_tags.specials[index].definition_index == weapon->definition_index &&
			weapon_tags.specials[index].kind == _pack_weapon_sword)
		{
			sword = TRUE;
		}
	}
	if (!sword)
		return;
	pressed = TEST_FLAG(*unit_control_flags, _unit_control_weapon_primary_trigger_bit) ||
		TEST_FLAG(*unit_control_flags, _unit_control_use_equipment_bit);
	SET_FLAG(*unit_control_flags, _unit_control_weapon_primary_trigger_bit, FALSE);
	if (pressed)
		SET_FLAG(*unit_control_flags, _unit_control_use_equipment_bit, TRUE);
	/* (the lunge, as the swing begins) */
	if (pressed && swinging_units[slot] != unit_index && unit->object.type == _object_type_biped)
	{
		struct object_iterator iterator;
		struct object_datum *other;
		long target = NONE;
		real nearest = SWORD_LUNGE_RANGE;

		object_iterator_new(&iterator, _object_mask_biped, 0);
		while ((other = object_iterator_next(&iterator)) != NULL)
		{
			real_vector3d to;
			real distance;

			if (iterator.index == unit_index || TEST_FLAG(other->object.damage_flags, _object_dead_bit) ||
				!game_team_is_enemy(unit->object.owner_team_index, other->object.owner_team_index))
			{
				continue;
			}
			to.i = other->object.position.x - unit->object.position.x;
			to.j = other->object.position.y - unit->object.position.y;
			to.k = other->object.position.z - unit->object.position.z;
			distance = magnitude3d(&to);
			if (distance < 0.01f || distance >= nearest)
				continue;
			if (dot_product3d(&to, &unit->unit.aiming_vector) / distance < SWORD_LUNGE_COSINE)
				continue;
			nearest = distance;
			target = iterator.index;
		}
		if (target != NONE && nearest > 1.0f)
		{
			struct object_datum *aimed = object_get(target);
			real_vector3d to;

			to.i = aimed->object.position.x - unit->object.position.x;
			to.j = aimed->object.position.y - unit->object.position.y;
			to.k = 0.0f;
			normalize3d(&to);
			unit->object.translational_velocity.i = to.i * SWORD_LUNGE_SPEED;
			unit->object.translational_velocity.j = to.j * SWORD_LUNGE_SPEED;
			unit->object.translational_velocity.k = MAX(unit->object.translational_velocity.k, 0.02f);
		}
	}
	swinging_units[slot] = pressed ? unit_index : NONE;
}
