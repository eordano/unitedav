// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;

namespace UnitedAV.Samples.EndToEnd
{
    public enum CaseStatus
    {
        Pending = 0,
        Pass = 1,
        Fail = 2,
        Skip = 3,
    }

    [Serializable]
    public class CaseResult
    {
        public string id;
        public string name;
        public string status;
        public string detail;
        public string blocker;
        public string remediation;
        public double durationMs;

        [NonSerialized] public CaseStatus statusEnum = CaseStatus.Pending;

        public void Set(CaseStatus s, string detailText = null,
                        string blockerText = null, string remediationText = null)
        {
            statusEnum = s;
            status = s.ToString();
            if (detailText != null) detail = detailText;
            if (blockerText != null) blocker = blockerText;
            if (remediationText != null) remediation = remediationText;
        }
    }

    [Serializable]
    public class BatteryReport
    {
        public int schema = 1;
        public string package = "org.unitedav";
        public string platform;
        public string unityVersion;
        public string graphicsDevice;
        public bool nativePluginLoaded;
        public string mediaPath;
        public string hwMediaPath;
        public string hwDecodeEnv;
        public string hwBackend = "";
        public bool overallPass;
        public int passCount;
        public int failCount;
        public int skipCount;
        public int exitCode;
        public List<CaseResult> cases = new List<CaseResult>();
    }
}
