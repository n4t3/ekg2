/*
 *  (C) Copyright 2000-2001 Richard Hughes, Roland Rabien, Tristan Van de Vreede
 *  (C) Copyright 2001-2002 Jon Keating, Richard Hughes
 *  (C) Copyright 2002-2004 Martin Öberg, Sam Kothari, Robert Rainwater
 *  (C) Copyright 2004-2008 Joe Kucera
 *
 * ekg2 port:
 *  (C) Copyright 2006-2008 Jakub Zawadzki <darkjames@darkjames.ath.cx>
 *                     2008 Wiesław Ochmiński <wiechu@wiechu.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

SNAC_SUBHANDLER(icq_snac_sigon_error) {		/* Miranda: OK */
	struct {
		uint16_t error;
	} pkt;
	uint16_t error;

	if (ICQ_UNPACK(&buf, "W", &pkt.error))
		error = pkt.error;
	else
		error = 0;

	icq_snac_error_handler(s, "sigon", error);
	return 0;
}

SNAC_SUBHANDLER(icq_snac_sigon_reply) {
	debug_function("icq_snac_sigon_reply()\n");

	return icq_flap_close_helper(s, buf, len);
}

extern char *icq_md5_digest(const char *password, const unsigned char *key, int key_len);	/* digest.c */

SNAC_SUBHANDLER(icq_snac_sigon_authkey) {
	/*
	 * This is the second snac in md5 crypted login sequence. Server send this for SNAC(17,06) request.
	 * This snac contain server generated auth key. Client should use it to crypt password.
	 */
	struct {
		uint16_t key_len;
	} pkt;
	string_t str;

	char *digest;

	if (!ICQ_UNPACK(&buf, "W", &pkt.key_len)) {
		icq_handle_disconnect(s, "Secure login failed. Invalid server response.", 0);		/* XXX */
		return -1;
	}

	if (!pkt.key_len || len < pkt.key_len) {
		icq_handle_disconnect(s, "Secure login failed. Invalid key length.", 0);		/* XXX */
		return -1;
	}

	/* XXX, miranda limit key to 64B */
	
	digest = icq_md5_digest(session_password_get(s), buf, pkt.key_len);

	str = string_init(NULL);

	icq_pack_append(str, "T", icq_pack_tlv_str(1, s->uid + 4));	/* uid */
	icq_pack_append(str, "T", icq_pack_tlv(0x25, digest, 16));	/* MD5-digest */
	icq_pack_append(str, "T", icq_pack_tlv(0x4C, NULL, 0));		/* empty TLV(0x4C): unknown */

	icq_pack_append_client_identification(str);			// Pack client identification details.

	icq_makesnac(s, str, 0x17, 2, 0, 0);
	icq_send_pkt(s, str);					// Send CLI_MD5_LOGIN 
	return 0;
}

SNAC_HANDLER(icq_snac_sigon_handler) {
	snac_subhandler_t handler;

	switch (cmd) {
		case 0x01: handler = icq_snac_sigon_error; break;	/* Miranda: OK */
		case 0x03: handler = icq_snac_sigon_reply; break;	/* Miranda: OK */
		case 0x07: handler = icq_snac_sigon_authkey; break;	/* Miranda: OK */
		default:   handler = NULL; break;
	}

	if (!handler) {
		debug_error("icq_snac_sigon_handler() SNAC with unknown cmd: %.4x received\n", cmd);
		icq_hexdump(DEBUG_ERROR, buf, len);
	} else
		handler(s, buf, len);

	return 0;
}
