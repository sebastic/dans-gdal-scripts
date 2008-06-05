/*
Copyright (c) 2007, Regents of the University of Alaska

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of the Geographic Information Network of Alaska nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/



#include "common.h"
#include "georef.h"
#include "default_palette.h"

// these are global so that they can be printed by the usage() function
float default_slope_exageration = 2.0;
float default_lightvec[] = { 0, 1, 1.5 };
float default_shade_params[] = { 0, 1, .5, 10 };

#define SHADE_TABLE_SIZE 500
#define SHADE_TABLE_SCALE 100.0
#define EARTH_RADIUS 6370997.0

typedef struct {
	int use_min;
	double min_val;
	int use_max;
	double max_val;
	int num_ndv;
	double *ndv;
} data_range_t;

void check_ndv(double *vals, char *is_ndv, int width, data_range_t *valid_range);
void scale_values(double *vals, int w, double scale, double offset);

typedef struct {
	unsigned char nan_red, nan_green, nan_blue;
	int num_vals;
	float *vals;
	unsigned char *reds, *greens, *blues;
} palette_t;

palette_t *read_palette_file(const char *fn);
palette_t *read_default_palette();
void get_nan_color(unsigned char *buf, palette_t *pal);
void get_palette_color(unsigned char *buf, float val, palette_t *pal);

void compute_tierow_invaffine(
	georef_t *georef,
	int num_cols, int row, int grid_spacing,
	double *invaffine_tierow
);

void usage(const char *cmdname) {
	printf("Usage: %s <options> src_dataset dst_dataset\n\n", cmdname);
	
	print_georef_usage();
	printf("\n");
	printf("Input/Output:\n");
	printf("  -b input_band_id\n");
	printf("  -of output_format\n");
	printf("  -ndv no_data_val [-ndv val ...]     Set a list of input no-data values\n");
	printf("  -min min_val -max max_val           Set range of valid input values\n");
	printf("  -offset X -scale X                  Multiply and add to source values\n");
	printf("\n");
	printf("Texture: (choose one of these - default is gray background)\n");
	printf("  -palette palette.pal                Palette file to map elevation values to colors\n");
	printf("  -default-palette                    Use the builtin default palette\n");
	printf("  -texture texture_image              Hillshade a given raster (must be same georeference as DEM)\n");
	printf("  -alpha-overlay                      Generate an RGBA image that can be used as a hillshade mask\n");
	printf("\n");
	printf("Shading:\n");
	printf("  -exag slope_exageration             Exagerate slope (default: %.1f)\n", default_slope_exageration);
	printf("  -shade ambient diffuse specular_intensity specular_falloff          (default: %.1f, %.1f, %.1f, %.1f)\n",
		default_shade_params[0], default_shade_params[1], default_shade_params[2], default_shade_params[3]);
	printf("  -lightvec sun_x sun_y sun_z                                         (default: %.1f, %.1f, %.1f)\n",
		default_lightvec[0], default_lightvec[1], default_lightvec[2]);
	printf("\n");
	printf("The -palette option creates a color-mapped image.  A default palette (dem.pal)\n");
	printf("is included in the distribution.  The -texture option is used for hillshading\n");
	printf("a raster image.  The texture and DEM must have the same geocoding.  The\n");
	printf("-alpha-overlay option generates an output with an alpha channel that can be\n");
	printf("drawn on top of a map to create a hillshaded image.\n");
	printf("\n");
	printf("If the DEM has projection information it will be used to ensure that the\n");
	printf("shading is done relative to true north.\n");
	printf("\n");
	exit(1);
}

int main(int argc, char *argv[]) {
	int i;

	float slope_exageration = default_slope_exageration;
	float lightvec[3];
	memcpy(lightvec, default_lightvec, 3*sizeof(float));
	float shade_params[4];
	memcpy(shade_params, default_shade_params, 4*sizeof(float));

	const char *src_fn = NULL;
	const char *tex_fn = NULL;
	const char *dst_fn = NULL;
	const char *palette_fn = NULL;
	int use_default_palette = 0;
	const char *output_format = NULL;
	int grid_spacing = 20; // could be configurable...
	int band_id = 1;
	double src_offset = 0;
	double src_scale = 1;
	int data24bit = 0;
	int alpha_overlay = 0;

	data_range_t valid_range;
	valid_range.use_min = 0;
	valid_range.use_max = 0;
	valid_range.num_ndv = 0;
	valid_range.ndv = NULL;

	geo_opts_t geo_opts = init_geo_options(&argc, &argv);

	int argp = 1;
	while(argp < argc) {
		char *arg = argv[argp++];
		// FIXME - check duplicate values
		if(arg[0] == '-') {
			if(!strcmp(arg, "-min")) {
				if(valid_range.use_min) fatal_error("min value specified twice");
				if(argp == argc) usage(argv[0]);
				char *endptr;
				valid_range.use_min = 1;
				valid_range.min_val = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-max")) {
				if(valid_range.use_max) fatal_error("max value specified twice");
				if(argp == argc) usage(argv[0]);
				char *endptr;
				valid_range.use_max = 1;
				valid_range.max_val = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-ndv")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				double val = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
				valid_range.ndv = (double *)realloc_or_die(valid_range.ndv,
					sizeof(double) * (valid_range.num_ndv+1));
				valid_range.ndv[valid_range.num_ndv] = val;
				valid_range.num_ndv++;
			} else if(!strcmp(arg, "-b")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				band_id = strtol(argv[argp++], &endptr, 10);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-palette")) {
				if(argp == argc) usage(argv[0]);
				palette_fn = argv[argp++];
				if(!strcmp(palette_fn, "data24bit")) {
					data24bit = 1;
				}
			} else if(!strcmp(arg, "-default-palette")) {
				use_default_palette = 1;
			} else if(!strcmp(arg, "-texture")) {
				if(argp == argc) usage(argv[0]);
				tex_fn = argv[argp++];
			} else if(!strcmp(arg, "-alpha-overlay")) {
				alpha_overlay++;
			} else if(!strcmp(arg, "-of")) {
				if(argp == argc) usage(argv[0]);
				output_format = argv[argp++];
			} else if(!strcmp(arg, "-exag")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				slope_exageration = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-lightvec")) {
				int i;
				for(i=0; i<3; i++) {
					if(argp == argc) usage(argv[0]);
					char *endptr;
					lightvec[i] = strtod(argv[argp++], &endptr);
					if(*endptr) usage(argv[0]);
				}
			} else if(!strcmp(arg, "-shade")) {
				int i;
				for(i=0; i<4; i++) {
					if(argp == argc) usage(argv[0]);
					char *endptr;
					shade_params[i] = strtod(argv[argp++], &endptr);
					if(*endptr) usage(argv[0]);
				}
			} else if(!strcmp(arg, "-offset")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				src_offset = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-scale")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				src_scale = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else usage(argv[0]);
		} else {
			if(src_fn && dst_fn) {
				usage(argv[0]);
			} else if(src_fn) {
				dst_fn = arg;
			} else {
				src_fn = arg;
			}
		}
	}

	if(!src_fn || !dst_fn) usage(argv[0]);

	if(!output_format) output_format = "GTiff";

	int num_output_modes = 
		(data24bit ? 1 : 0) +
		(use_default_palette ? 1 : 0) +
		(palette_fn ? 1 : 0) +
		(alpha_overlay ? 1 : 0);
	if(num_output_modes > 1) fatal_error("you can only use one of the -palette, -default-palette, -texture, and -alpha-overlay options");

	GDALAllRegister();

	//////// open palette ////////

	palette_t *palette = NULL;
	int do_shade;
	int out_numbands;

	if(data24bit) {
		do_shade = 0;
		out_numbands = 3;
	} else if(use_default_palette) {
		palette = read_default_palette();
		do_shade = 1;
		out_numbands = 3;
	} else if(palette_fn) {
		palette = read_palette_file(palette_fn);
		if(!palette) fatal_error("palette was null"); // this can't happen
		do_shade = 1;
		out_numbands = 3;
	} else if(alpha_overlay) {
		do_shade = 1;
		out_numbands = 4;
	} else {
		do_shade = 1;
		out_numbands = 1;
	}

	//////// open DEM ////////

	GDALDatasetH src_ds = GDALOpen(src_fn, GA_ReadOnly);
	if(!src_ds) fatal_error("open failed");

	GDALRasterBandH src_band = GDALGetRasterBand(src_ds, band_id);

	int w = GDALGetRasterXSize(src_ds);
	int h = GDALGetRasterYSize(src_ds);
	if(!w || !h) fatal_error("missing width/height");
	printf("Input size is %d, %d\n", w, h);

	georef_t georef = init_georef(&geo_opts, src_ds);

	//////// compute orientation ////////

	double *constant_invaffine = NULL;
	if(do_shade) {
		if(!georef.fwd_xform) {
			if(!georef.inv_affine) fatal_error("please specify resolution of image");
			printf("warning: no SRS available - basing orientation on affine transform\n");
			constant_invaffine = (double *)malloc_or_die(sizeof(double) * 4);
			constant_invaffine[0] = georef.inv_affine[1];
			constant_invaffine[1] = georef.inv_affine[2];
			constant_invaffine[2] = georef.inv_affine[4];
			constant_invaffine[3] = georef.inv_affine[5];
		}
	}

	//////// open texture ////////

	GDALDatasetH tex_ds = NULL;
	GDALRasterBandH *tex_bands = NULL;
	if(tex_fn) {
		tex_ds = GDALOpen(tex_fn, GA_ReadOnly);
		if(!tex_ds) fatal_error("open failed");

		int tex_w = GDALGetRasterXSize(tex_ds);
		int tex_h = GDALGetRasterYSize(tex_ds);
		if(!tex_w || !tex_h) fatal_error("missing width/height for texture");
		if(tex_w != w || tex_h != h) fatal_error("DEM and texture are different sizes");

		out_numbands = GDALGetRasterCount(tex_ds);
		tex_bands = (GDALRasterBandH *)malloc_or_die(sizeof(GDALRasterBandH) * out_numbands);
		for(i=0; i<out_numbands; i++) {
			tex_bands[i] = GDALGetRasterBand(tex_ds, i+1);
			if(!tex_bands[i]) fatal_error("could not open texture band");
		}
	}

	//////// open output ////////

	GDALDriverH dst_driver = GDALGetDriverByName(output_format);
	if(!dst_driver) fatal_error("unrecognized output format (%s)", output_format);
	GDALDatasetH dst_ds = GDALCreate(dst_driver, dst_fn, w, h, out_numbands, GDT_Byte, NULL);
	if(!dst_ds) fatal_error("could create dst_dataset");

	if(georef.fwd_affine) {
		GDALSetGeoTransform(dst_ds, georef.fwd_affine);
	}
	GDALSetProjection(dst_ds, GDALGetProjectionRef(src_ds));

	GDALRasterBandH *dst_band = (GDALRasterBandH *)malloc_or_die(sizeof(GDALRasterBandH) * out_numbands);
	for(i=0; i<out_numbands; i++) {
		dst_band[i] = GDALGetRasterBand(dst_ds, i+1);
	}

	//////// setup shade table ////////

	int row, col;
	float shade_table[SHADE_TABLE_SIZE*2+1][SHADE_TABLE_SIZE*2+1];
	float spec_table[SHADE_TABLE_SIZE*2+1][SHADE_TABLE_SIZE*2+1];
	float ALPHA_THRESH = .5F;
	float thresh_brite = 1;
	if(do_shade) {
		float lightvec_len = sqrt(
			lightvec[0] * lightvec[0] +
			lightvec[1] * lightvec[1] +
			lightvec[2] * lightvec[2]);
		if(lightvec_len < 1e-6) fatal_error("lightvec cannot be zero");
		lightvec[0] /= lightvec_len;
		lightvec[1] /= lightvec_len;
		lightvec[2] /= lightvec_len;
		for(row=0; row<SHADE_TABLE_SIZE*2+1; row++) {
			for(col=0; col<SHADE_TABLE_SIZE*2+1; col++) {
				float dx = (float)(col-SHADE_TABLE_SIZE) / SHADE_TABLE_SCALE;
				float dy = (float)(row-SHADE_TABLE_SIZE) / SHADE_TABLE_SCALE;
				float vx, vy, vz;
				if(dx==0 && dy==0) {
					vx = vy = 0;
					vz = 1;
				} else {
					float s = sqrt(1.0 + dx*dx + dy*dy);
					vx = dx / s;
					vy = dy / s;
					vz = 1.0 / s;
				}
				float dotprod = vx*lightvec[0] + vy*lightvec[1] + vz*lightvec[2];
				if(dotprod < 0) dotprod = 0;
				float brite = shade_params[0] + shade_params[1]*dotprod;
				if(brite > 1.0) brite = 1.0;
				shade_table[row][col] = brite;
				brite = shade_params[2]*powf(dotprod, shade_params[3]);
				spec_table[row][col] = brite;
			}
		}
					
		if(shade_params[3] > 0) {
			// this stuff is to ensure that the mask is
			// fully bright when the transition is made
			// from diffuse to specular
			float thresh_dotprod = powf(ALPHA_THRESH / shade_params[2], 1.0F / shade_params[3]);
			thresh_brite = shade_params[0] + shade_params[1] * thresh_dotprod;
			if(thresh_brite > 1) thresh_brite = 1;
		}
	}

	//////// process image ////////

	int gdal_have_ndv;
	double gdal_ndv = GDALGetRasterNoDataValue(src_band, &gdal_have_ndv);
	if(gdal_have_ndv) {
		valid_range.ndv = (double *)realloc_or_die(valid_range.ndv,
			sizeof(double) * (valid_range.num_ndv+1));
		valid_range.ndv[valid_range.num_ndv] = gdal_ndv;
		valid_range.num_ndv++;
	}

	double *inbuf_prev = (double *)malloc_or_die(sizeof(double) * w);
	double *inbuf_this = (double *)malloc_or_die(sizeof(double) * w);
	double *inbuf_next = (double *)malloc_or_die(sizeof(double) * w);
	char *inbuf_ndv_prev = (char *)malloc_or_die(w);
	char *inbuf_ndv_this = (char *)malloc_or_die(w);
	char *inbuf_ndv_next = (char *)malloc_or_die(w);

	unsigned char **outbuf = (unsigned char **)malloc_or_die(sizeof(unsigned char *) * out_numbands);
	for(i=0; i<out_numbands; i++) {
		outbuf[i] = (unsigned char *)malloc_or_die(w);
	}
	unsigned char *pixel = (unsigned char *)malloc_or_die(out_numbands);

	double *invaffine_tierow_above=NULL, *invaffine_tierow_below=NULL;
	if(do_shade && !constant_invaffine) {
		invaffine_tierow_above = (double *)malloc_or_die(sizeof(double) * w * 4);
		invaffine_tierow_below = (double *)malloc_or_die(sizeof(double) * w * 4);
	}

	double min=0, max=0; // initialized to prevent compiler warning
	int got_nan=0, got_valid=0, got_overflow=0;
	for(row=0; row<h; row++) {
		GDALTermProgress((double)row / (double)h, NULL, NULL);
		if(row == 0) {
			GDALRasterIO(src_band, GF_Read, 0, 0, w, 1, inbuf_prev, w, 1, GDT_Float64, 0, 0);
			GDALRasterIO(src_band, GF_Read, 0, 0, w, 1, inbuf_this, w, 1, GDT_Float64, 0, 0);
			GDALRasterIO(src_band, GF_Read, 0, 1, w, 1, inbuf_next, w, 1, GDT_Float64, 0, 0);
			check_ndv(inbuf_prev, inbuf_ndv_prev, w, &valid_range);
			check_ndv(inbuf_this, inbuf_ndv_this, w, &valid_range);
			check_ndv(inbuf_next, inbuf_ndv_next, w, &valid_range);
			scale_values(inbuf_prev, w, src_scale, src_offset);
			scale_values(inbuf_this, w, src_scale, src_offset);
			scale_values(inbuf_next, w, src_scale, src_offset);
		} else {
			double *swapd = inbuf_prev;
			inbuf_prev = inbuf_this;
			inbuf_this = inbuf_next;
			inbuf_next = swapd;
			char *swapc = inbuf_ndv_prev;
			inbuf_ndv_prev = inbuf_ndv_this;
			inbuf_ndv_this = inbuf_ndv_next;
			inbuf_ndv_next = swapc;
			if(row == h-1) {
				GDALRasterIO(src_band, GF_Read, 0, row, w, 1, inbuf_next, w, 1, GDT_Float64, 0, 0);
			} else {
				GDALRasterIO(src_band, GF_Read, 0, row+1, w, 1, inbuf_next, w, 1, GDT_Float64, 0, 0);
			}
			check_ndv(inbuf_next, inbuf_ndv_next, w, &valid_range);
			scale_values(inbuf_next, w, src_scale, src_offset);
		}
		if(tex_bands) {
			for(i=0; i<out_numbands; i++) {
				GDALRasterIO(tex_bands[i], GF_Read, 0, row, w, 1, outbuf[i], w, 1, GDT_Byte, 0, 0);
			}
		}

		double grid_fraction = 0;
		if(do_shade && !constant_invaffine) {
			// the part sets up the bilinear interpolation of invaffine
			int above_tiept = grid_spacing * (int)(row / grid_spacing);
			int below_tiept = above_tiept + grid_spacing;
			if(below_tiept > h) below_tiept = h;
			if(row == above_tiept) {
				if(row == 0) {
					compute_tierow_invaffine(&georef, w, 0, grid_spacing, invaffine_tierow_above);
				} else {
					double *tmp = invaffine_tierow_above;
					invaffine_tierow_above = invaffine_tierow_below;
					invaffine_tierow_below = tmp;
				}
				compute_tierow_invaffine(&georef, w, below_tiept, grid_spacing, invaffine_tierow_below);
			}
			double segment_height = below_tiept - above_tiept;
			grid_fraction = ((double)row - (double)above_tiept) / segment_height;
		}
//if(!row) printf("pixel[0,0]=%f\n", inbuf_this[0]);
		for(col=0; col<w; col++) {
			double val = inbuf_this[col];
			float brite, spec;
			if(do_shade) {
				double dx;
				int mid_good = !inbuf_ndv_this[col];
				int left_good = col>0 && !inbuf_ndv_this[col-1];
				int right_good = col<w-1 && !inbuf_ndv_this[col+1];
				int up_good = row>0 && !inbuf_ndv_prev[col];
				int down_good = row<h-1 && !inbuf_ndv_next[col];
				if(left_good && right_good) {
					dx = (inbuf_this[col+1] - inbuf_this[col-1]) / 2.0;
				} else if(mid_good && right_good) {
					dx = inbuf_this[col+1] - val;
				} else if(mid_good && left_good) {
					dx = val - inbuf_this[col-1];
				} else {
					dx = 0;
				}
				double dy;
				if(up_good && down_good) {
					dy = (inbuf_next[col] - inbuf_prev[col]) / 2.0;
				} else if(mid_good && down_good) {
					dy = inbuf_next[col] - val;
				} else if(mid_good && up_good) {
					dy = val - inbuf_prev[col];
				} else {
					dy = 0;
				}

				// convert from elevation per pixel to elevation per meter (unitless)
				//double dx2 = invaffine_a * dx + invaffine_b * (-dy);
				//double dy2 = invaffine_c * dx + invaffine_d * (-dy);
				
				double invaffine[4];
				int i;
				if(constant_invaffine) {
					for(i=0; i<4; i++) invaffine[i] = constant_invaffine[i];
				} else {
					for(i=0; i<4; i++) {
						invaffine[i] = invaffine_tierow_above[col*4 + i] * (1.0 - grid_fraction) +
							invaffine_tierow_below[col*4 + i] * grid_fraction;
					}
					//compute_invaffine(georef, col, row, invaffine);
				}
				// FIXME - why the minus signs?
				double dx2 = invaffine[0] * (-dx) + invaffine[1] * (-dy);
				double dy2 = invaffine[2] * (-dx) + invaffine[3] * (-dy);
				dx2 *= slope_exageration;
				dy2 *= slope_exageration;
				//if(col == 0) printf("row=%d d=[%f, %f] d2=[%f, %f]\n", row, dx, dy, dx2, dy2);

				int st_col = SHADE_TABLE_SIZE + (int)(SHADE_TABLE_SCALE * dx2);
				if(st_col < 0) st_col = 0;
				if(st_col > SHADE_TABLE_SIZE*2) st_col = SHADE_TABLE_SIZE*2;
				int st_row = SHADE_TABLE_SIZE + (int)(SHADE_TABLE_SCALE * dy2);
				if(st_row < 0) st_row = 0;
				if(st_row > SHADE_TABLE_SIZE*2) st_row = SHADE_TABLE_SIZE*2;
				brite = shade_table[st_row][st_col];
				spec = spec_table[st_row][st_col];
			} else {
				brite = 1.0;
				spec = 0.0;
			}
			if(inbuf_ndv_this[col]) {
				if(palette) {
					get_nan_color(pixel, palette);
					for(i=0; i<out_numbands; i++) outbuf[i][col] = pixel[i];
				} else {
					for(i=0; i<out_numbands; i++) outbuf[i][col] = 0;
				}
				got_nan=1;
			} else {
				if(data24bit) {
					int ival = (int)round(val) + (1<<23);
					if(ival >> 24) {
						got_overflow = 1;
						ival = 0;
					}
					pixel[2] = ival & 0xff;
					pixel[1] = (ival >> 8) & 0xff;
					pixel[0] = (ival >> 16) & 0xff;
				} else if(alpha_overlay) {
					if(thresh_brite < 1.0) {
						brite += (spec / ALPHA_THRESH) * (1.0 - thresh_brite);
					}

					float alpha, white;
					if(spec < ALPHA_THRESH) {
						alpha = 1.0 - brite;
						white = 0;
					} else {
						alpha = spec - ALPHA_THRESH;
						white = 1;
					}

					if(alpha < 0) alpha = 0;
					if(alpha > 1) alpha = 1;
					if(white < 0) white = 0;
					if(white > 1) white = 1;

					pixel[0] = pixel[1] = pixel[2] = (int)(255.0 * white);
					pixel[3] = (int)(255.0 * alpha);
				} else {
					if(palette) {
						get_palette_color(pixel, val, palette);
					} else if(tex_ds) {
						for(i=0; i<out_numbands; i++) pixel[i] = outbuf[i][col];
					} else {
						for(i=0; i<out_numbands; i++) pixel[i] = 128;
					}

					for(i=0; i<out_numbands; i++) {
						int c = pixel[i];
						c = (int)(c * brite + 255.0 * spec);
						if(c > 254) c = 254;
						pixel[i] = c;
					}
				}
				for(i=0; i<out_numbands; i++) outbuf[i][col] = pixel[i];

				if(!got_valid || val < min) min = val;
				if(!got_valid || val > max) max = val;
				got_valid=1;
			}
		}
		for(i=0; i<out_numbands; i++) {
			GDALRasterIO(dst_band[i], GF_Write, 0, row, w, 1, outbuf[i], w, 1, GDT_Byte, 0, 0);
		}
	}

	GDALTermProgress(1, NULL, NULL);

	if(tex_ds) GDALClose(tex_ds);
	GDALClose(src_ds);
	GDALClose(dst_ds);

	printf("got_nan=%d, got_valid=%d, min=%f, max=%f\n",
		got_nan, got_valid, min, max);
	if(got_overflow) {
		printf("got an overflow in conversion to 24-bit\n");
	}

	return 0;
}

void check_ndv(double *vals, char *is_ndv, int width, data_range_t *valid_range) {
	int i, j;
	for(i=0; i<width; i++) is_ndv[i] = 0;
	if(valid_range->use_min) {
		double min = valid_range->min_val;
		for(i=0; i<width; i++) {
			if(vals[i] < min) is_ndv[i] = 1;
		}
	}
	if(valid_range->use_max) {
		double max = valid_range->max_val;
		for(i=0; i<width; i++) {
			if(vals[i] > max) is_ndv[i] = 1;
		}
	}
	for(j=0; j<valid_range->num_ndv; j++) {
		double ndv = valid_range->ndv[j];
		for(i=0; i<width; i++) {
			if(vals[i] == ndv) is_ndv[i] = 1;
		}
	}
}

void scale_values(double *vals, int w, double scale, double offset) {
	double *p = vals;
	while(w--) {
		*p = (*p) * scale + offset;
		p++;
	}
}

palette_t *parse_palette(const char * const *lines) {
	palette_t *p = (palette_t *)malloc_or_die(sizeof(palette_t));

	int num = 0;
	p->vals = NULL;
	p->reds = p->greens = p->blues = NULL;
	p->nan_red = p->nan_green = p->nan_blue = 0;

	for(int line_num=0; lines[line_num]; line_num++) {
		const char *line = lines[line_num];
		int r, g, b;
		float val;
		if(4 != sscanf(line, "%f %d %d %d\n", &val, &r, &g, &b)) {
			fatal_error("cannot parse line in palette file: [%s]", line);
		}
		if(isnan(val)) {
			p->nan_red   = r;
			p->nan_green = g;
			p->nan_blue  = b;
		} else {
			num++;
			p->vals = (float *)realloc_or_die(p->vals, num*sizeof(float));
			p->reds   = (unsigned char *)realloc_or_die(p->reds,   num);
			p->greens = (unsigned char *)realloc_or_die(p->greens, num);
			p->blues  = (unsigned char *)realloc_or_die(p->blues,  num);
			p->vals  [num-1] = val;
			p->reds  [num-1] = r;
			p->greens[num-1] = g;
			p->blues [num-1] = b;
		}
	}
	
	if(num < 2) fatal_error("not enough entries in palette");
	p->num_vals = num;

	return p;
}

palette_t *read_palette_file(const char *fn) {
	FILE *fh = fopen(fn, "r");
	if(!fh) fatal_error("cannot open palette file");
	char **lines = NULL;
	int num_lines = 0;
	char *line;
	char buf[1000];
	while((line = fgets(buf, 1000, fh))) {
		lines = (char **)realloc_or_die(lines, sizeof(char *) * (num_lines + 2));
		lines[num_lines] = strdup(line);
		lines[num_lines+1] = NULL;
		num_lines++;
	}
	fclose(fh);

	if(!num_lines) fatal_error("palette file was empty");

	palette_t *p = parse_palette(lines);

	for(int i=0; lines[i]; i++) free(lines[i]);
	free(lines);

	return p;
}

palette_t *read_default_palette() {
	return parse_palette(DEFAULT_PALETTE);
}

void get_nan_color(unsigned char *buf, palette_t *pal) {
	buf[0] = pal->nan_red;
	buf[1] = pal->nan_green;
	buf[2] = pal->nan_blue;
}

void get_palette_color(unsigned char *buf, float val, palette_t *pal) {
	int i;
	float v1, v2, alpha;

	if(val < pal->vals[0]) val = pal->vals[0];
	if(val > pal->vals[pal->num_vals-1]) val = pal->vals[pal->num_vals-1];

	for(i=0; i<pal->num_vals-1; i++) {
		v1 = pal->vals[i];
		v2 = pal->vals[i+1];
		if(val >= v1 && val <= v2) {
			alpha = (val - v1) / (v2 - v1);
			buf[0] = (int)((float)pal->reds  [i]*(1.0-alpha) + (float)pal->reds  [i+1]*alpha + .5);
			buf[1] = (int)((float)pal->greens[i]*(1.0-alpha) + (float)pal->greens[i+1]*alpha + .5);
			buf[2] = (int)((float)pal->blues [i]*(1.0-alpha) + (float)pal->blues [i+1]*alpha + .5);
			return;
		}
	}

	fatal_error("palette file out of sequence\n");
}

// this function generates a 2x2 matrix that
// can be used to convert row/column gradients
// to easting/northing gradients
void compute_invaffine(
	georef_t *georef, double col, double row, double *invaffine
) {
	// in case we return due to error:
	invaffine[0] = invaffine[1] =
		invaffine[2] = invaffine[3] = 0;

	// we want to compute d{longitude}/d{easting} etc.
	// for the given pixel (row/col)
	double epsilon = 1;
	double lon_0, lat_0;
	xy2ll(georef, col, row, &lon_0, &lat_0);
	double lon_dx, lat_dx;
	xy2ll(georef, col+epsilon, row, &lon_dx, &lat_dx);
	double lon_dy, lat_dy;
	xy2ll(georef, col, row+epsilon, &lon_dy, &lat_dy);

	if(
		lon_0  == HUGE_VAL || lat_0  == HUGE_VAL ||
		lon_dx == HUGE_VAL || lat_dx == HUGE_VAL ||
		lon_dy == HUGE_VAL || lat_dy == HUGE_VAL
	) {
		//printf("proj error\n");
		// error - return [0,0,0,0]
		return;
	}

	//	printf("ll=[%f, %f] [%f, %f] [%f, %f]\n", 
	//		lon_0, lat_0, lon_dx, lat_dx, lon_dy, lat_dy);

	// here the derivative is computed:
	lon_dx -= lon_0;   lat_dx -= lat_0;
	// undo phase wrap
	if(lon_dx >  300) lon_dx -= 360.0;
	if(lon_dx < -300) lon_dx += 360.0;
	// divide dlon and dlat by dx
	lon_dx /= epsilon; lat_dx /= epsilon;

	lon_dy -= lon_0;   lat_dy -= lat_0;
	// undo phase wrap
	if(lon_dy >  300) lon_dy -= 360.0;
	if(lon_dy < -300) lon_dy += 360.0;
	// divide dlon and dlat by dy
	lon_dy /= epsilon; lat_dy /= epsilon;

	// convert lon/lat infinitesimals to true east/north infinitesimals
	double te_dx = cos(lat_0 * D2R) * lon_dx * D2R * EARTH_RADIUS;
	double tn_dx = lat_dx * D2R * EARTH_RADIUS;
	double te_dy = cos(lat_0 * D2R) * lon_dy * D2R * EARTH_RADIUS;
	double tn_dy = lat_dy * D2R * EARTH_RADIUS;

	// invert the affine matrix:

	// a b    te_dx te_dy
	// c d    tn_dx tn_dy

	double s = te_dx * tn_dy - te_dy * tn_dx; // ad - bc
	// characteristic length
	// if s << l then dx and dy are parallel in the north/east plane
	double l = (te_dx*te_dx + te_dy*te_dy + tn_dx*tn_dx + tn_dy*tn_dy) / 2.0;
	if(fabs(s) < fabs(l) / 100.0 || s == 0.0) {
		//printf("cannot invert affine matrix [%f, %f], [%f, %f, %f, %f]\n",
		//	col, row, lon_dx, lon_dy, lat_dx, lat_dy);
		// error - return [0,0,0,0]
		return;
	}

	invaffine[0] =  tn_dy / s; //  d/s
	invaffine[1] = -te_dy / s; // -b/s
	invaffine[2] = -tn_dx / s; // -c/s
	invaffine[3] =  te_dx / s; //  a/s

	//printf("invaffine=[%f, %f, %f, %f]\n", invaffine_a, invaffine_b, invaffine_c, invaffine_d);
}

// interpolate the invaffine for an entire row
void compute_tierow_invaffine(
	georef_t *georef,
	int num_cols, int row, int grid_spacing,
	double *invaffine_tierow
) {
	int i;
	double tiecol_left[4];
	double tiecol_right[4];

	compute_invaffine(georef, 0, row, tiecol_right);

	int col;
	double segment_width = 0; // will be initialized on first iteration
	for(col=0; col<num_cols; col++) {
		int left_tiept = grid_spacing * (int)(col / grid_spacing);
		if(col == left_tiept) {
			for(i=0; i<4; i++) tiecol_left[i] = tiecol_right[i];
			int right_tiept = col+grid_spacing;
			if(right_tiept > num_cols) right_tiept = num_cols;
			segment_width = right_tiept - left_tiept;
			compute_invaffine(georef, right_tiept, row, tiecol_right);
		}
		double grid_fraction = ((double)col - (double)left_tiept) / segment_width;
		for(i=0; i<4; i++) {
			invaffine_tierow[col*4 + i] =
				tiecol_left[i] * (1.0 - grid_fraction) +
				tiecol_right[i] * grid_fraction;
		}
	}
}
