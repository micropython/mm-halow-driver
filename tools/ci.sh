#!/bin/bash

########################################################################################
# code formatting

function ci_code_formatting_setup {
    sudo apt-get install uncrustify
    pip3 install black
    uncrustify --version
    black --version
}

function ci_code_formatting_run {
    tools/codeformat.py -v
}

########################################################################################
# code spelling

function ci_code_spell_setup {
    pip3 install codespell
}

function ci_code_spell_run {
    # src/ and tests/ arrive with the driver; spell-check whatever is present.
    codespell README.md $(test -d src && echo src) $(test -d tests && echo tests)
}
