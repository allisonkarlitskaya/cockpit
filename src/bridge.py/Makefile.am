INJECTOR_SH_IN = src/bridge.py/injector.sh.in
BRIDGE_PY = src/bridge.py/bridge.py

src/bridge.py/injector.sh: $(INJECTOR_SH_IN) $(BRIDGE_PY)
	$(AM_V_GEN) sed "s|@script@|$$(xz -e < $(BRIDGE_PY) | base64 -w 0)|" $< > $@
