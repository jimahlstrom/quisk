.PHONY: quisk

quisk:
	python3 setup.py build_ext --force --inplace
	@echo

quisk3:
	python3 setup.py build_ext --force --inplace
	@echo

soapy3:
	(cd soapypkg; make soapy3)

afedrinet3:
	(cd afedrinet; make afedrinet3)

perseus3:
	(cd perseuspkg; make perseus3)
