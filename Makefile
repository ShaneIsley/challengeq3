VERSION=1.33_SVN$(shell LANG=C svnversion .)

release debug clean:
	$(MAKE) -C code/unix $@

dist:
	rm -rf quake3-$(VERSION)
	svn export . quake3-$(VERSION)
	tar --force-local -cjf quake3-$(VERSION).tar.bz2 quake3-$(VERSION)
	rm -rf quake3-$(VERSION)

.PHONY: release debug clean
