.PHONY: all run clean

elf := gpiot

objs += gpiot.o

DEPDIR := deps
$(shell mkdir -p $(DEPDIR) >/dev/null)

V:=@

DEFINES +=
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td
C_FLAGS += -g3 -O0 $(DEFINES) $(DEPFLAGS)
POSTCOMPILE = mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@

run: $(elf)
	./$<

all: $(elf)

clean:
	@echo "RM"
	$(V)rm -f *.elf *.o *.d *.deps
	$(V)rm -r $(DEPDIR)

$(elf): $(objs)
	@echo "LD $(@F)"
	$(V)$(CROSS_COMPILE)g++ $(C_FLAGS) -o $@ $?

%.o: %.cpp
	@echo "CC $(@F)"
	$(V)$(CROSS_COMPILE)g++ $(C_FLAGS) -c -o $@ $<
	$(V)$(POSTCOMPILE)

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.

include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(obj:.o=.c))))
