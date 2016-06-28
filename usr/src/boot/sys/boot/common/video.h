
struct edid_header {
	uint8_t header[8];	/* fixed header pattern */
	uint16_t manufacturer_id;
	uint16_t product_code;
	uint32_t serial_number;
	uint8_t week_of_manufacture;
	uint8_t year_of_manufacture;
	uint8_t version;
	uint8_t revision;
};

struct edid_basic_display_parameters {
	uint8_t video_input_parameters;
	uint8_t max_horizontal_image_size;
	uint8_t max_vertical_image_size;
	uint8_t display_gamma;
	uint8_t supported_features;
};

struct edid_chromaticity_coordinates {
	uint8_t red_green_lo;
	uint8_t blue_white_lo;
	uint8_t red_x_hi;
	uint8_t red_y_hi;
	uint8_t green_x_hi;
	uint8_t green_y_hi;
	uint8_t blue_x_hi;
	uint8_t blue_y_hi;
	uint8_t white_x_hi;
	uint8_t white_y_hi;
};

struct edid_detailed_timings {
	uint16_t pixel_clock;
	uint8_t horizontal_active_lo;
	uint8_t horizontal_blanking_lo;
	uint8_t horizontal_hi;
	uint8_t vertical_active_lo;
	uint8_t vertical_blanking_lo;
	uint8_t vertical_hi;
	uint8_t horizontal_sync_offset_lo;
	uint8_t horizontal_sync_pulse_width_lo;
	uint8_t vertical_sync_lo;
	uint8_t sync_hi;
	uint8_t horizontal_image_size_lo;
	uint8_t vertical_image_size_lo;
	uint8_t image_size_hi;
	uint8_t horizontal_border;
	uint8_t vertical_border;
	uint8_t features;
};

struct vesa_edid_info {
	struct edid_header header;
	struct edid_basic_display_parameters display;
#define	EDID_FEATURE_PREFERRED_TIMING_MODE	(1 << 1)
	struct edid_chromaticity_coordinates chromaticity;
	uint8_t established_timings_1;
	uint8_t established_timings_2;
	uint8_t manufacturer_reserved_timings;
	uint16_t standard_timings[8];
	struct edid_detailed_timings detailed_timings[4];
	uint8_t number_of_extensions;
	uint8_t checksum;
} __packed;
