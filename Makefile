# CMP 50HX unlock — one-shot local release (mirrors .github/workflows/release.yml)
#
#   make release       build the Linux .run + the Windows zip locally
#   make efi           build just the Linux EFI binary
#   make win           build just the Windows Go tools + zip
#   make clean         remove all build outputs

CARGO_OS_LINUX  := linux/amd64
INSTALLER       := cmp50hx-unlock-linux.run
PAYLOAD_TGZ     := cmp50hx-unlock-linux.tar.gz
WIN_ZIP         := cmp50hx-unlock-windows.zip
STAGE_DIR       := .release-stage
VERSION         ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

.PHONY: all release release-linux efi win clean

all: release

# ------------------------------------------------------------------
# Linux side
# ------------------------------------------------------------------
efi:
	cd efi-unlock && ./build.sh

$(PAYLOAD_TGZ): efi
	rm -rf $(STAGE_DIR); mkdir -p $(STAGE_DIR)
	cp efi-unlock/install-linux.sh                          $(STAGE_DIR)/install.sh
	cp efi-unlock/sign-efi.sh                               $(STAGE_DIR)/
	cp efi-unlock/50HXUNLK.EFI                              $(STAGE_DIR)/
	sha256sum efi-unlock/50HXUNLK.EFI                       > $(STAGE_DIR)/50HXUNLK.EFI.sha256
	cp efi-unlock/README.md                                 $(STAGE_DIR)/
	tar -C $(STAGE_DIR) -czf $(PAYLOAD_TGZ) .
	sha256sum $(PAYLOAD_TGZ) > $(PAYLOAD_TGZ).sha256
	@echo "wrote $(PAYLOAD_TGZ) ($$(wc -c < $(PAYLOAD_TGZ)) bytes)"

# The hash must be taken inside this recipe: the tarball depends on the phony
# "efi" target, so it is rebuilt as part of this same run and a hash computed
# at parse time is stale ("SHA mismatch" on every clean `make release`).
$(INSTALLER): $(PAYLOAD_TGZ)
	@sha=$$(sha256sum $(PAYLOAD_TGZ) | awk '{print $$1}'); \
	CMP_VERSION=$(VERSION) \
	CMP_RELEASE_URL="file://$$(pwd)/$(PAYLOAD_TGZ)" \
	CMP_RELEASE_SHA="$$sha" \
	  bash tools/build-linux-installer.sh \
	    --tar $(PAYLOAD_TGZ) \
	    --out $(INSTALLER) \
	    --sha256 "$$sha"
	@echo "wrote $(INSTALLER) ($$(wc -c < $(INSTALLER)) bytes)"

# ------------------------------------------------------------------
# Windows side (requires Go 1.23+ on PATH; PowerShell for the zip)
# ------------------------------------------------------------------
win:
	@command -v go >/dev/null 2>&1 || (echo "need go 1.23+ on PATH" && exit 2)
	@command -v pwsh >/dev/null 2>&1 || (echo "need pwsh on PATH" && exit 2)
	./tools/build-windows.ps1 -Version $(VERSION) -Source . -Out $(WIN_ZIP)

# ------------------------------------------------------------------
# Aggregate
# ------------------------------------------------------------------
# Linux side only: no Go or pwsh required, so this works on a bare host.
release-linux: $(INSTALLER)
	@echo
	@echo "===== Linux installer ====="
	ls -la $(INSTALLER) $(PAYLOAD_TGZ) $(PAYLOAD_TGZ).sha256
	@echo "User install: sudo ./$(INSTALLER)"

release: $(INSTALLER) win
	@echo
	@echo "===== Linux installer ====="
	ls -la $(INSTALLER) $(PAYLOAD_TGZ) $(PAYLOAD_TGZ).sha256
	@echo
	@echo "===== Windows bundle ====="
	ls -la $(WIN_ZIP) 2>/dev/null || echo "(Windows build skipped — install Go + pwsh)"
	@echo
	@echo "User install (Linux):   sudo ./$(INSTALLER)"
	@echo "User install (Win):    unzip and run 50HXInstaller.exe as admin"

clean:
	rm -f efi-unlock/50HXUNLK.EFI efi-unlock/*.o efi-unlock/*.so
	rm -rf $(STAGE_DIR)
	rm -f $(INSTALLER) $(PAYLOAD_TGZ) $(PAYLOAD_TGZ).sha256 $(WIN_ZIP)
	rm -rf artifacts/
