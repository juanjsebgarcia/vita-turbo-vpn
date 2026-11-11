# Root certificate bundle

`cacert.pem` is Mozilla's set of trusted root certificates, as extracted and
published by the curl project at https://curl.se/docs/caextract.html. The
app embeds it so its HTTPS checks work on every firmware without iTLS-Enso.
Users can add roots of their own in
`ux0:data/vita-turbo-vpn/extra-ca.pem`.

Licence: the certificate data comes from Mozilla's `certdata.txt`, under the
Mozilla Public License 2.0.

<!-- The update workflow rewrites the three lines below; keep their format. -->
- Source: https://curl.se/ca/cacert.pem
- Mozilla data as of: Tue Oct 20 13:32:40 2026 GMT
- File SHA-256: 796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe

## Updates

When curl publishes a new bundle, replace `cacert.pem`, verify the checksum,
check that every certificate parses, and review the roots added and removed:
a removed root is a service that will stop passing the check. The next app
build carries the new bundle.

Checked on 2026-10-29: the default exit-IP service (via ISRG Root X1/X2),
`api.ipify.org` and `ifconfig.co` (via GTS Root R4) all verify against this
file alone.

The workflow lives in this repository along with this
folder.
