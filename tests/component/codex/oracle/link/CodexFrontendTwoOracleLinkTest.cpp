bool legacyServerOracleLinkProbe();
bool legacyClientOracleLinkProbe();

int main() {
    return legacyServerOracleLinkProbe() && legacyClientOracleLinkProbe() ? 0 : 1;
}
