#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

#include "tsfuncs.h"

struct ts_pat *ts_pat_alloc() {
	struct ts_pat *pat = calloc(1, sizeof(struct ts_pat));
	pat->section_header	= ts_section_data_alloc();
	pat->programs_max	= 128;
	pat->programs		= calloc(pat->programs_max, sizeof(void *));
	return pat;
}

void ts_pat_free(struct ts_pat **ppat) {
	struct ts_pat *pat = *ppat;
	int i;
	if (pat) {
		ts_section_data_free(&pat->section_header);
		for (i=0;i<pat->programs_num;i++) {
			FREE(pat->programs[i]);
		}
		FREE(pat->programs);
		FREE(*ppat);
	}
}

static struct ts_pat *ts_pat_reset(struct ts_pat *pat) {
	struct ts_pat *newpat = ts_pat_alloc();
	ts_pat_free(&pat);
	return newpat;
}

struct ts_pat *ts_pat_push_packet(struct ts_pat *pat, uint8_t *ts_packet) {
	struct ts_header ts_header;
	memset(&ts_header, 0, sizeof(struct ts_header));

	if (ts_packet_header_parse(ts_packet, &ts_header)) {
		// PAT should be with PID 0x00
		if (ts_header.pid != 0x00)
			goto OUT;
		if (!pat->ts_header.pusi)
			pat->ts_header = ts_header;
	}

	if (ts_header.pusi) {
		struct ts_section_header section_header;
		memset(&section_header, 0, sizeof(struct ts_section_header));

		uint8_t *section_data = ts_section_header_parse(ts_packet, &pat->ts_header, &section_header);
		if (!section_data || !section_header.section_syntax_indicator) {
			memset(&pat->ts_header, 0, sizeof(struct ts_header));
			goto OUT;
		}
		// table_id should be 0x00 (program_association_section)
		if (section_header.table_id != 0x00) {
			memset(&pat->ts_header, 0, sizeof(struct ts_header));
			goto OUT;
		}

		// Set correct section_header
		ts_section_header_parse(ts_packet, &pat->ts_header, pat->section_header);
	}

	if (!pat->initialized) {
		if (pat->section_header->section_syntax_indicator) {
			ts_section_add_packet(pat->section_header, &ts_header, ts_packet);
			if (pat->section_header->initialized) {
				if (!ts_pat_parse(pat))
					goto ERROR;
			}
		}
	}

OUT:
	return pat;

ERROR:
	return ts_pat_reset(pat);
}

int ts_pat_parse(struct ts_pat *pat) {
	uint8_t *section_data = pat->section_header->section_data + 8; // + 8 to compensate for section table header
	int section_len = pat->section_header->packet_section_len;

	while (section_len > 0) {
		if (pat->programs_num == pat->programs_max) {
			ts_LOGf("PAT contains too many programs (>%d), not all are initialized!\n", pat->programs_max);
			break;
		}
		struct ts_pat_program *pinfo = calloc(1, sizeof(struct ts_pat_program));

		pinfo->program  = (section_data[0] << 8) | section_data[1];				// xxxxxxxx xxxxxxxx
		pinfo->reserved = (section_data[2] &~ 0x1F) >> 5;						// xxx11111
		pinfo->pid      = ((section_data[2] &~ 0xE0) << 8) | section_data[3];	// 111xxxxx xxxxxxxx

		pat->programs[pat->programs_num] = pinfo;
		pat->programs_num++;

		section_data += 4;
		section_len  -= 4;
	}
	pat->CRC = (pat->CRC << 8) | section_data[3];
	pat->CRC = (pat->CRC << 8) | section_data[2];
	pat->CRC = (pat->CRC << 8) | section_data[1];
	pat->CRC = (pat->CRC << 8) | section_data[0];

	u_int32_t check_crc = ts_crc32(pat->section_header->section_data, pat->section_header->data_size);
	if (check_crc != 0) {
		ts_LOGf("!!! Wrong PAT CRC! It should be 0 but it is %08x (CRC in data is 0x%08x)\n", check_crc, pat->CRC);
		return 0;
	}

	pat->initialized = 1;
	return 1;
}

void ts_pat_generate(struct ts_pat *pat, uint8_t **ts_packets, int *num_packets) {
	uint8_t *secdata = ts_section_data_alloc_section();
	ts_section_header_generate(secdata, pat->section_header, 0);
	int curpos = 8; // Compensate for the section header, frist data byte is at offset 8

	int i;
	for (i=0;i<pat->programs_num;i++) {
		struct ts_pat_program *prg = pat->programs[i];
		secdata[curpos + 0] = prg->program >> 8;
		secdata[curpos + 1] = prg->program &~ 0xff00;

		secdata[curpos + 2]  = prg->reserved << 5;
		secdata[curpos + 2] |= prg->pid >> 8;
		secdata[curpos + 3]  = prg->pid &~ 0xff00;
		curpos += 4; // Compensate for the above
	}
	pat->CRC = ts_section_data_calculate_crc(secdata, curpos);
	curpos += 4; // CRC

	ts_section_data_gen_ts_packets(&pat->ts_header, secdata, curpos, pat->section_header->pointer_field, ts_packets, num_packets);

	FREE(secdata);
}

void ts_pat_regenerate_packets(struct ts_pat *pat) {
	uint8_t *ts_packets;
	int num_packets;
	ts_pat_generate(pat, &ts_packets, &num_packets);
	FREE(pat->section_header->packet_data);
	pat->section_header->packet_data = ts_packets;
	pat->section_header->num_packets = num_packets;
}

struct ts_pat *ts_pat_copy(struct ts_pat *pat) {
	struct ts_pat *newpat = ts_pat_alloc();
	int i;
	for (i=0;i<pat->section_header->num_packets; i++) {
		newpat = ts_pat_push_packet(newpat, pat->section_header->packet_data + (i * TS_PACKET_SIZE));
	}
	if (newpat->initialized) {
		return newpat;
	} else {
		ts_LOGf("Error copying PAT!\n");
		ts_pat_free(&newpat);
		return NULL;
	}
}

void ts_pat_check_generator(struct ts_pat *pat) {
	struct ts_pat *pat1 = ts_pat_copy(pat);
	if (pat1) {
		ts_compare_data("PAT (tspacket->struct)",
			pat1->section_header->packet_data,
			pat->section_header->packet_data,
			pat->section_header->num_packets * TS_PACKET_SIZE);
		ts_pat_free(&pat1);
	}

	uint8_t *ts_packets;
	int num_packets;
	ts_pat_generate(pat, &ts_packets, &num_packets);
	if (num_packets != pat->section_header->num_packets) {
		ts_LOGf("ERROR: num_packets:%d != sec->num_packets:%d\n", num_packets, pat->section_header->num_packets);
	}
	ts_compare_data("PAT (struct->tspacket)", pat->section_header->packet_data, ts_packets, num_packets * TS_PACKET_SIZE);
	free(ts_packets);
}

void ts_pat_dump(struct ts_pat *pat) {
	int i;
	ts_LOGf("PAT packet\n");
    for(i=0;i<pat->section_header->num_packets;i++) {
        struct ts_header tshdr;
        ts_packet_header_parse(pat->section_header->packet_data + (i * TS_PACKET_SIZE), &tshdr);
        ts_packet_header_dump(&tshdr);
    }
    ts_section_header_dump(pat->section_header);
	ts_LOGf("  * PAT data\n");
	ts_LOGf("    * num_programs: %d\n", pat->programs_num);
	for (i=0;i<pat->programs_num;i++) {
		struct ts_pat_program *prg = pat->programs[i];
		ts_LOGf("      * [%02d/%02d]: Program No 0x%04x (%5d) -> PID %04x (%d) /res: 0x%02x/\n",
			i+1, pat->programs_num,
			prg->program, prg->program,
			prg->pid, prg->pid,
			prg->reserved);
		// Program number 0 is Network ID, not program id
		if (prg->program == 0) {
			ts_LOGf("      - NIT PID %04x (%d)\n", prg->pid, prg->pid);
		}
	}
	ts_LOGf("  * CRC 0x%08x\n", pat->CRC);

	ts_pat_check_generator(pat);
}

int ts_pat_is_same(struct ts_pat *pat1, struct ts_pat *pat2) {
	int i;

	if (pat1->CRC == pat2->CRC) // Same
		return 1;

	// If some version is not current, just claim the structures are the same
	if (!pat1->section_header->current_next_indicator || pat2->section_header->version_number)
		return 1;

	if (pat1->section_header->version_number != pat2->section_header->version_number) // Different
		return 0;

	if (pat1->programs_num != pat2->programs_num) // Different
		return 0;

	// Check each program and PIDs
	for (i=0;i<pat1->programs_num;i++) {
		struct ts_pat_program *prg1 = pat1->programs[i];
		struct ts_pat_program *prg2 = pat2->programs[i];
		if (prg1->program != prg2->program || prg1->pid != prg2->pid) // Different
			return 0;
	}

	return 1; // Same
}