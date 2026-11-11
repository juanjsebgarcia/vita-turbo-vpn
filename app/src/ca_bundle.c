/*
 * ca_bundle.c - Mozilla's root certificates (third_party/ca-bundle/
 * cacert.pem) built into the app, as the HTTPS check's trust anchors.
 *
 * The file goes in as it is, with .incbin, plus the NUL byte that mbedTLS's
 * PEM parser expects at the end. CA_PEM is its path, set by CMakeLists.txt,
 * which also makes this file rebuild when the bundle changes.
 */
#ifndef CA_PEM
#error "CA_PEM must name cacert.pem"
#endif

__asm__(
	".section .rodata.vpnturbo_ca_pem,\"a\",%progbits\n"
	".global vpnturbo_ca_pem\n"
	".global vpnturbo_ca_pem_end\n"
	".type vpnturbo_ca_pem, %object\n"
	".balign 4\n"
	"vpnturbo_ca_pem:\n"
	".incbin \"" CA_PEM "\"\n"
	".byte 0\n"
	"vpnturbo_ca_pem_end:\n"
	".previous\n"
);
