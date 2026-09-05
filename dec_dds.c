/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / DDS (DirectDraw Surface) decoder filter. The
 *  block decompression comes from bcdec.h (https://github.com/iOrange/bcdec),
 *  a single-header library compiled directly into this filter (no prebuilt
 *  static library, same as qdbmp/qoi in this repo). The DDS container itself
 *  is parsed here.
 *
 *  Covers BC1-BC7. Only the first surface (mip level 0) is decoded; mipmaps,
 *  cubemaps and volume textures are ignored.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#define DDS_MAGIC 0x20534444u /* "DDS " little-endian */

enum
{
	DDS_BC1 = 1,
	DDS_BC2,
	DDS_BC3,
	DDS_BC4,
	DDS_BC5,
	DDS_BC6H,
	DDS_BC7
};

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_DDSDecCtx;

static u32 dds_u32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Returns one of the DDS_BCx codes, 0 if the pixel format is not a block
 * compressed one we handle. is_signed is set for the signed BC6H variant. */
static u32 dds_parse(const u8 *data, u32 size, u32 *width, u32 *height, u32 *data_offset, Bool *is_signed)
{
	u32 fourcc, pf_flags, hdr_size;

	if (size < 128 || dds_u32(data) != DDS_MAGIC)
		return 0;
	hdr_size = dds_u32(data + 4);
	if (hdr_size != 124)
		return 0;

	*height = dds_u32(data + 12);
	*width = dds_u32(data + 16);
	pf_flags = dds_u32(data + 80);
	fourcc = dds_u32(data + 84);
	*data_offset = 128;
	*is_signed = GF_FALSE;

	if (!(pf_flags & 0x4 /* DDPF_FOURCC */))
		return 0;

	switch (fourcc)
	{
	case 0x31545844: /* DXT1 */
		return DDS_BC1;
	case 0x33545844: /* DXT3 */
		return DDS_BC2;
	case 0x35545844: /* DXT5 */
		return DDS_BC3;
	case 0x31495441: /* ATI1 */
	case 0x55344342: /* BC4U */
		return DDS_BC4;
	case 0x32495441: /* ATI2 */
	case 0x55354342: /* BC5U */
		return DDS_BC5;
	case 0x30315844: /* DX10: the real format is in the extension header */
	{
		u32 dxgi;
		if (size < 148)
			return 0;
		dxgi = dds_u32(data + 128);
		*data_offset = 148;
		switch (dxgi)
		{
		case 70: case 71: case 72:  return DDS_BC1;  /* BC1_TYPELESS/UNORM/UNORM_SRGB */
		case 73: case 74: case 75:  return DDS_BC2;
		case 76: case 77: case 78:  return DDS_BC3;
		case 79: case 80:           return DDS_BC4;
		case 81:                    return DDS_BC4;  /* BC4_SNORM, decoded unsigned */
		case 82: case 83:           return DDS_BC5;
		case 84:                    return DDS_BC5;  /* BC5_SNORM */
		case 94: case 95:           return DDS_BC6H;
		case 96:                    *is_signed = GF_TRUE; return DDS_BC6H;
		case 97: case 98: case 99:  return DDS_BC7;
		default:                    return 0;
		}
	}
	default:
		return 0;
	}
}

static GF_Err ddsdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_DDSDecCtx *ctx = (GF_DDSDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	return GF_OK;
}

static Bool ddsdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_DDSDecCtx *ctx = (GF_DDSDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static u8 dds_clamp_float(float v)
{
	/* BC6H is high dynamic range; this is a plain clamp, not a tone mapper -
	 * values above 1.0 saturate. */
	if (v <= 0.f)
		return 0;
	if (v >= 1.f)
		return 255;
	return (u8)(v * 255.f + 0.5f);
}

static GF_Err ddsdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, out_size, width = 0, height = 0, offset = 0, format, nb_comp;
	u32 bx, by, blocks_x, blocks_y, block_size;
	Bool is_signed = GF_FALSE;
	const u8 *src;
	GF_DDSDecCtx *ctx = (GF_DDSDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	format = dds_parse(data, size, &width, &height, &offset, &is_signed);
	if (!format || !width || !height)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DDSDec] Unsupported or invalid DDS file\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	/* Always RGB on the output: an RGBA pid has no adaptation path to writegen
	 * in this build ("No suitable filter to adapt caps"), the same limitation
	 * documented for the rgba mode in test-player/libpng.js. The alpha channel
	 * BC1/BC2/BC3/BC7 may carry is therefore dropped. */
	nb_comp = 3;
	switch (format)
	{
	case DDS_BC1: block_size = BCDEC_BC1_BLOCK_SIZE; break;
	case DDS_BC2: block_size = BCDEC_BC2_BLOCK_SIZE; break;
	case DDS_BC3: block_size = BCDEC_BC3_BLOCK_SIZE; break;
	case DDS_BC4: block_size = BCDEC_BC4_BLOCK_SIZE; break;
	case DDS_BC5: block_size = BCDEC_BC5_BLOCK_SIZE; break;
	case DDS_BC6H: block_size = BCDEC_BC6H_BLOCK_SIZE; break;
	default: block_size = BCDEC_BC7_BLOCK_SIZE; break;
	}

	/* Block compressed formats always cover a multiple of 4 pixels; a
	 * non-multiple size means the last blocks are partly cropped. */
	blocks_x = (width + 3) / 4;
	blocks_y = (height + 3) / 4;
	if (size < offset + blocks_x * blocks_y * block_size)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DDSDec] Truncated DDS payload\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	out_size = width * height * nb_comp;

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(width * nb_comp));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	memset(output, 0, out_size);

	src = data + offset;
	for (by = 0; by < blocks_y; by++)
	{
		for (bx = 0; bx < blocks_x; bx++)
		{
			/* Decoded through a 4x4 scratch block and copied afterwards:
			 * blocks on the right/bottom edge may hang over the image. */
			u8 blk[4 * 4 * 4];
			float blkf[4 * 4 * 3];
			u32 px, py;

			switch (format)
			{
			case DDS_BC1: bcdec_bc1(src, blk, 4 * 4); break;
			case DDS_BC2: bcdec_bc2(src, blk, 4 * 4); break;
			case DDS_BC3: bcdec_bc3(src, blk, 4 * 4); break;
			case DDS_BC4: bcdec_bc4(src, blk, 4 * 1); break;
			case DDS_BC5: bcdec_bc5(src, blk, 4 * 2); break;
			case DDS_BC6H: bcdec_bc6h_float(src, blkf, 4 * 3, is_signed ? 1 : 0); break;
			default: bcdec_bc7(src, blk, 4 * 4); break;
			}
			src += block_size;

			for (py = 0; py < 4; py++)
			{
				u32 iy = by * 4 + py;
				if (iy >= height)
					break;
				for (px = 0; px < 4; px++)
				{
					u32 ix = bx * 4 + px;
					u8 *dst;
					if (ix >= width)
						break;
					dst = output + (iy * width + ix) * nb_comp;
					switch (format)
					{
					case DDS_BC4:
					{
						u8 v = blk[py * 4 + px];
						dst[0] = dst[1] = dst[2] = v; /* single channel shown as grey */
						break;
					}
					case DDS_BC5:
						dst[0] = blk[(py * 4 + px) * 2];
						dst[1] = blk[(py * 4 + px) * 2 + 1];
						dst[2] = 0;
						break;
					case DDS_BC6H:
						dst[0] = dds_clamp_float(blkf[(py * 4 + px) * 3]);
						dst[1] = dds_clamp_float(blkf[(py * 4 + px) * 3 + 1]);
						dst[2] = dds_clamp_float(blkf[(py * 4 + px) * 3 + 2]);
						break;
					default:
						/* bcdec writes RGBA blocks; only RGB is kept. */
						memcpy(dst, blk + (py * 4 + px) * 4, 3);
						break;
					}
				}
			}
		}
	}
	gf_filter_pid_drop_packet(ctx->ipid);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void ddsdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability DDSDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "dds"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/vnd-ms.dds|image/x-dds"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister DDSDecoderRegister = {
	.name = "ddsdec",
	GF_FS_SET_DESCRIPTION("DDS (DirectDraw Surface) decoder")
		GF_FS_SET_HELP("This filter decodes BC1 to BC7 compressed DDS textures using bcdec.")
			.private_size = sizeof(GF_DDSDecCtx),
	SETCAPS(DDSDecCaps),
	.configure_pid = ddsdec_configure_pid,
	.process = ddsdec_process,
	.process_event = ddsdec_process_event,
	.finalize = ddsdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE ddsdec_register(GF_FilterSession *session)
{
	return &DDSDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_ddsdec(void) {
    gf_filter_auto_register("ddsdec", ddsdec_register);
}
