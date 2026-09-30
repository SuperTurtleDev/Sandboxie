//
//  Values are 32 bit values laid out as follows:
//
//   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
//   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
//  +---+-+-+-----------------------+-------------------------------+
//  |Sev|C|R|     Facility          |               Code            |
//  +---+-+-+-----------------------+-------------------------------+
//
//  where
//
//      Sev - is the severity code
//
//          00 - Success
//          01 - Informational
//          10 - Warning
//          11 - Error
//
//      C - is the Customer code flag
//
//      R - is a reserved bit
//
//      Facility - is the facility code
//
//      Code - is the facility's status code
//
//
// Define the facility codes
//
#define MSG_FACILITY_EVENT               0x101
#define MSG_FACILITY_POPUP               0x102


//
// Define the severity codes
//


//
// MessageId: MSG_1101
//
// MessageText:
//
// SBIE1101 Sandboxie driver (SbieDrv) version %2 initialized
//
#define MSG_1101                         0x4101044DL

//
// MessageId: MSG_1102
//
// MessageText:
//
// SBIE1102 Sandboxie driver (SbieDrv) unloading
//
#define MSG_1102                         0x4101044EL

//
// MessageId: MSG_1103
//
// MessageText:
//
// SBIE1103 Sandboxie driver (SbieDrv) version %2 failed to start
//
#define MSG_1103                         0xC101044FL

//
// MessageId: MSG_1104
//
// MessageText:
//
// SBIE1104 Insufficient system resources to complete initialization
//
#define MSG_1104                         0xC1010450L

//
// MessageId: MSG_1105
//
// MessageText:
//
// SBIE1105 Unknown operating system version:  %2
//
#define MSG_1105                         0xC1010451L

//
// MessageId: MSG_1106
//
// MessageText:
//
// SBIE1106 error %2, detail %3
//
#define MSG_1106                         0xC1010452L

//
// MessageId: MSG_1108
//
// MessageText:
//
// SBIE1108 Procedure %2 could not be analysed
//
#define MSG_1108                         0xC1010454L

//
// MessageId: MSG_1110
//
// MessageText:
//
// SBIE1110 Cannot intercept type %3, error %2
//
#define MSG_1110                         0xC1010456L

//
// MessageId: MSG_1111
//
// MessageText:
//
// SBIE1111 System DLL %3 could not be loaded %2
//
#define MSG_1111                         0xC1010457L

//
// MessageId: MSG_1112
//
// MessageText:
//
// SBIE1112 Procedure %2 could not be located
//
#define MSG_1112                         0xC1010458L

//
// MessageId: MSG_1113
//
// MessageText:
//
// SBIE1113 Cannot find Nt system service, reason %2
//
#define MSG_1113                         0xC1010459L

//
// MessageId: MSG_1114
//
// MessageText:
//
// SBIE1114 Cannot find Zw system service, reason %2
//
#define MSG_1114                         0xC101045AL

//
// MessageId: MSG_1116
//
// MessageText:
//
// SBIE1116 Driver failed to register process notification routine %2
//
#define MSG_1116                         0xC101045CL

//
// MessageId: MSG_1119
//
// MessageText:
//
// SBIE1119 Cannot create API device %2
//
#define MSG_1119                         0xC101045FL

//
// MessageId: MSG_1120
//
// MessageText:
//
// SBIE1120 Mismatch in service %2
//
#define MSG_1120                         0xC1010460L

//
// MessageId: MSG_1121
//
// MessageText:
//
// SBIE1121 Hook failed for service %2
//
#define MSG_1121                         0xC1010461L

//
// MessageId: MSG_1122
//
// MessageText:
//
// SBIE1122 Error:  %2
//
#define MSG_1122                         0xC1010462L

//
// MessageId: MSG_1151
//
// MessageText:
//
// SBIE1151 Cannot handle instruction %2
//
#define MSG_1151                         0xC103047FL

//
// MessageId: MSG_1152
//
// MessageText:
//
// SBIE1152 Trampoline allocation failed %2
//
#define MSG_1152                         0xC1030480L

//
// MessageId: MSG_1153
//
// MessageText:
//
// SBIE1153 Sandboxie initialization failed.  Close all programs and then re-install Sandboxie  OR  restart your computer.
//
#define MSG_1153                         0xC1030481L

//
// MessageId: MSG_1205
//
// MessageText:
//
// SBIE1205 Invalid DynData found in registry, error: %2
//
#define MSG_1205                         0xC10304B5L

//
// MessageId: MSG_1206
//
// MessageText:
//
// SBIE1206 Your Windows build (%2) is not yet supported by sandboxie, error: %3
//
#define MSG_1206                         0xC10304B6L

//
// MessageId: MSG_1201
//
// MessageText:
//
// SBIE1201 Not enough memory
//
#define MSG_1201                         0xC10204B1L

//
// MessageId: MSG_1203
//
// MessageText:
//
// SBIE1203 Cannot build path list (error in %2)
//
#define MSG_1203                         0xC10204B3L

//
// MessageId: MSG_1204
//
// MessageText:
//
// SBIE1204 Sandbox creation failed for %3 %2
//
#define MSG_1204                         0xC10204B4L

//
// MessageId: MSG_1207
//
// MessageText:
//
// SBIE1207 Your Windows build (%2) is not yet supported by Sandboxie, which means applications will run without security isolation!
//
#define MSG_1207                         0xC10204B7L

//
// MessageId: MSG_1211
//
// MessageText:
//
// SBIE1211 Could not initiate sandboxing for process %2
//
#define MSG_1211                         0xC10204BBL

//
// MessageId: MSG_1212
//
// MessageText:
//
// SBIE1212 Cannot create directory '%3' %2
//
#define MSG_1212                         0xC10204BCL

//
// MessageId: MSG_1213
//
// MessageText:
//
// SBIE1213 Cannot create object directory '%3' %2
//
#define MSG_1213                         0xC10204BDL

//
// MessageId: MSG_1222
//
// MessageText:
//
// SBIE1222 Error with security token:  %2
//
#define MSG_1222                         0xC10204C6L

//
// MessageId: MSG_1231
//
// MessageText:
//
// SBIE1231 Initialization failed for process %3 %2
//
#define MSG_1231                         0xC10204CFL

//
// MessageId: MSG_1241
//
// MessageText:
//
// SBIE1241 Cannot mount registry hive:  %2
//
#define MSG_1241                         0xC10204D9L

//
// MessageId: MSG_1242
//
// MessageText:
//
// SBIE1242 Monitor buffer overflow
//
#define MSG_1242                         0xC10204DAL

//
// MessageId: MSG_1301
//
// MessageText:
//
// SBIE1301 Program '%2' was launched outside of the sandbox
//
#define MSG_1301                         0x41020515L

//
// MessageId: MSG_1305
//
// MessageText:
//
// SBIE1305 Blocked sandboxed image from being loaded - %2
//
#define MSG_1305                         0x41020519L

//
// MessageId: MSG_1306
//
// MessageText:
//
// SBIE1306 Sandboxie driver (SbieDrv) cannot be unloaded now
//
#define MSG_1306                         0x4102051AL

//
// MessageId: MSG_1307
//
// MessageText:
//
// SBIE1307 Program cannot access the Internet due to restrictions - %2
//
#define MSG_1307                         0x4102051BL

//
// MessageId: MSG_1308
//
// MessageText:
//
// SBIE1308 Program cannot start due to restrictions - %2
//
#define MSG_1308                         0x4102051CL

//
// MessageId: MSG_1309
//
// MessageText:
//
// SBIE1309 Denied attempt to change account password
//
#define MSG_1309                         0x4102051DL

//
// MessageId: MSG_1312
//
// MessageText:
//
// SBIE1312 Blocked request to start a 16-bit DOS program in the sandbox
//
#define MSG_1312                         0x41020520L

//
// MessageId: MSG_1313
//
// MessageText:
//
// SBIE1313 Blocked direct disk access by process '%2'
//
#define MSG_1313                         0x41020521L

//
// MessageId: MSG_1314
//
// MessageText:
//
// SBIE1314 Blocked request to alter network/firewall settings by process '%2'
//
#define MSG_1314                         0x41020522L

//
// MessageId: MSG_1316
//
// MessageText:
//
// SBIE1316 Blocked request to generate device event in the sandbox
//
#define MSG_1316                         0x41020524L

//
// MessageId: MSG_1317
//
// MessageText:
//
// SBIE1317 Blocked '%2' from trying to access sandbox file root '%3'
//
#define MSG_1317                         0x41020525L

//
// MessageId: MSG_1318
//
// MessageText:
//
// SBIE1318 Blocked '%2' from trying to access sandboxed process '%3'
//
#define MSG_1318                         0x41020526L

//
// MessageId: MSG_1319
//
// MessageText:
//
// SBIE1319 Blocked spooler print to file, %2 %3
//
#define MSG_1319                         0x41020527L

//
// MessageId: MSG_1320
//
// MessageText:
//
// SBIE1320 To allow print spooler to write outside the sandbox for this process, please double-click on this message line
//
#define MSG_1320                         0x81020528L

//
// MessageId: MSG_1321
//
// MessageText:
//
// SBIE1321 Program '%2' was forced into sandbox %3
//
#define MSG_1321                         0x41020529L

//
// MessageId: MSG_1399
//
// MessageText:
//
// %0
//
#define MSG_1399                         0x41020577L

//
// MessageId: MSG_1401
//
// MessageText:
//
// SBIE1401 Configuration file not found, using defaults
//
#define MSG_1401                         0x41030579L

//
// MessageId: MSG_1402
//
// MessageText:
//
// SBIE1402 Configuration file error in line %3:  %2
//
#define MSG_1402                         0x8103057AL

//
// MessageId: MSG_1403
//
// MessageText:
//
// SBIE1403 Configuration file error in line %2:  line too long
//
#define MSG_1403                         0x8103057BL

//
// MessageId: MSG_1404
//
// MessageText:
//
// SBIE1404 Configuration file error in line %2:  too many lines
//
#define MSG_1404                         0x8103057CL

//
// MessageId: MSG_1405
//
// MessageText:
//
// SBIE1405 Configuration file error in line %2:  syntax error
//
#define MSG_1405                         0x8103057DL

//
// MessageId: MSG_1406
//
// MessageText:
//
// SBIE1406 Missing or invalid expansion for %3:  %2
//
#define MSG_1406                         0xC103057EL

//
// MessageId: MSG_1408
//
// MessageText:
//
// SBIE1408 Unknown user name for SID:  %2
//
#define MSG_1408                         0xC1020580L

//
// MessageId: MSG_1409
//
// MessageText:
//
// SBIE1409 The Templates.ini file cannot be opened %2
//
#define MSG_1409                         0x81030581L

//
// MessageId: MSG_1410
//
// MessageText:
//
// SBIE1410 The following message indicates an error in the Templates.ini file
//
#define MSG_1410                         0x81030582L

//
// MessageId: MSG_1411
//
// MessageText:
//
// SBIE1411 Sandbox %2 specifies unknown template %3
//
#define MSG_1411                         0x81030583L

//
// MessageId: MSG_1412
//
// MessageText:
//
// SBIE1412 In text: %2
//
#define MSG_1412                         0xC1030584L

//
// MessageId: MSG_1413
//
// MessageText:
//
// SBIE1413 The box include folder %3 file cannot be opened, error %2
//
#define MSG_1413                         0x81030585L

//
// MessageId: MSG_1414
//
// MessageText:
//
// SBIE1414 The following message indicates an error %2 in a box include %3
//
#define MSG_1414                         0x81030586L

//
// MessageId: MSG_1415
//
// MessageText:
//
// SBIE1415 Portable box name mismatch (ini name must match box section) in a box include %3
//
#define MSG_1415                         0x81030587L

//
// MessageId: MSG_1416
//
// MessageText:
//
// SBIE1416 Portable box name is already in use by another box, include %3
//
#define MSG_1416                         0x81030588L

//
// MessageId: MSG_2101
//
// MessageText:
//
// SBIE2101 Object name not found: %3, error %2
//
#define MSG_2101                         0x41020835L

//
// MessageId: MSG_2102
//
// MessageText:
//
// SBIE2102 File is too large to copy into sandbox - %2
//
#define MSG_2102                         0x41020836L

//
// MessageId: MSG_2103
//
// MessageText:
//
// SBIE2103 Denied attempt to load system driver '%2'
//
#define MSG_2103                         0x41020837L

//
// MessageId: MSG_2104
//
// MessageText:
//
// SBIE2104 Denied attempt to end this Windows session - %2
//
#define MSG_2104                         0x41020838L

//
// MessageId: MSG_2108
//
// MessageText:
//
// SBIE2108 Faking successful completion for program '%2'
//
#define MSG_2108                         0x4102083CL

//
// MessageId: MSG_2111
//
// MessageText:
//
// SBIE2111 Process is not accessible: %3, call %2
//
#define MSG_2111                         0x4102083FL

//
// MessageId: MSG_2112
//
// MessageText:
//
// SBIE2112 Object is not accessible: %3, call %2
//
#define MSG_2112                         0x41020840L

//
// MessageId: MSG_2113
//
// MessageText:
//
// SBIE2113 File is too large to copy into sandbox, creating empty file - %2
//
#define MSG_2113                         0x41020841L

//
// MessageId: MSG_2114
//
// MessageText:
//
// SBIE2114 File is too large to copy into sandbox, denying access - %2
//
#define MSG_2114                         0x41020842L

//
// MessageId: MSG_2115
//
// MessageText:
//
// SBIE2115 File is too large to copy into sandbox, opening in read only - %2
//
#define MSG_2115                         0x41020843L

//
// MessageId: MSG_2180
//
// MessageText:
//
// SBIE2180 LowLevel.dll error %2
//
#define MSG_2180                         0x41020884L

//
// MessageId: MSG_2181
//
// MessageText:
//
// SBIE2181 LowLevel.dll detour failed to load SbieDll.dll into target process.
//
#define MSG_2181                         0x41020885L

//
// MessageId: MSG_2189
//
// MessageText:
//
// SBIE2189 %2 is likely a Chromium-based application, but the image type is not Chrome; it will likely fail.
//
#define MSG_2189                         0x4102088DL

//
// MessageId: MSG_2191
//
// MessageText:
//
// SBIE2191 %2 should not be updated while running under Sandboxie.
//
#define MSG_2191                         0x4102088FL

//
// MessageId: MSG_2192
//
// MessageText:
//
// SBIE2192 To update the program, run it outside of the supervision of Sandboxie.
//
#define MSG_2192                         0x41020890L

//
// MessageId: MSG_2193
//
// MessageText:
//
// SBIE2193 Make sure to delete the sandbox after completing the update process.
//
#define MSG_2193                         0x41020891L

//
// MessageId: MSG_2194
//
// MessageText:
//
// SBIE2194 MSI installer requires %2 option to be set in the ini to be able to work correctly, however this option weakens the isolation.
//
#define MSG_2194                         0x41020892L

//
// MessageId: MSG_2195
//
// MessageText:
//
// SBIE2195 To run Explorer.exe sandboxed, the access for COM infrastructure must not be Open.
//
#define MSG_2195                         0x41020893L

//
// MessageId: MSG_2196
//
// MessageText:
//
// SBIE2196 To run the MSI Installer sandboxed, the access for COM infrastructure must not be Open.
//
#define MSG_2196                         0x41020894L

//
// MessageId: MSG_2198
//
// MessageText:
//
// %0
//
#define MSG_2198                         0x41020896L

//
// MessageId: MSG_2199
//
// MessageText:
//
// %0
//
#define MSG_2199                         0x41020897L

//
// MessageId: MSG_2201
//
// MessageText:
//
// SBIE2201 %2
//
#define MSG_2201                         0x81020899L

//
// MessageId: MSG_2203
//
// MessageText:
//
// SBIE2203 Failed to communicate with Sandboxie Service:  %2
//
#define MSG_2203                         0x8102089BL

//
// MessageId: MSG_2204
//
// MessageText:
//
// SBIE2204 Cannot start sandboxed service %2
//
#define MSG_2204                         0x8102089CL

//
// MessageId: MSG_2205
//
// MessageText:
//
// SBIE2205 Service not implemented:  %2
//
#define MSG_2205                         0x8102089DL

//
// MessageId: MSG_2206
//
// MessageText:
//
// SBIE2206 Failed processing AutoExec setting %2
//
#define MSG_2206                         0x8102089EL

//
// MessageId: MSG_2207
//
// MessageText:
//
// SBIE2207 Invalid value for setting '%2', using default
//
#define MSG_2207                         0x8102089FL

//
// MessageId: MSG_2208
//
// MessageText:
//
// SBIE2208 Cannot remove registry hive:  %2
//
#define MSG_2208                         0x810208A0L

//
// MessageId: MSG_2209
//
// MessageText:
//
// SBIE2209 Cannot translate SID to user name:  %2
//
#define MSG_2209                         0x810208A1L

//
// MessageId: MSG_2210
//
// MessageText:
//
// SBIE2210 Cannot start Windows Explorer for:  %2
//
#define MSG_2210                         0x810208A2L

//
// MessageId: MSG_2211
//
// MessageText:
//
// SBIE2211 Sandboxed service failed to start:  %2
//
#define MSG_2211                         0x810208A3L

//
// MessageId: MSG_2212
//
// MessageText:
//
// SBIE2212 Email reader '%2' is not configured to run sandboxed
//
#define MSG_2212                         0x810208A4L

//
// MessageId: MSG_2213
//
// MessageText:
//
// SBIE2213 Windows Credentials cannot be stored in the sandbox
//
#define MSG_2213                         0x810208A5L

//
// MessageId: MSG_2214
//
// MessageText:
//
// SBIE2214 Request to start service '%2' was denied due to dropped rights
//
#define MSG_2214                         0x810208A6L

//
// MessageId: MSG_2217
//
// MessageText:
//
// SBIE2217 Request to run as Administrator was denied due to dropped rights
//
#define MSG_2217                         0x810208A9L

//
// MessageId: MSG_2218
//
// MessageText:
//
// SBIE2218 Failed to get elevated privileges:  %2
//
#define MSG_2218                         0x810208AAL

//
// MessageId: MSG_2219
//
// MessageText:
//
// SBIE2219 Request was issued by program %2
//
#define MSG_2219                         0x810208ABL

//
// MessageId: MSG_2220
//
// MessageText:
//
// SBIE2220 To permit use of Administrator privileges, please double-click on this message line
//
#define MSG_2220                         0x810208ACL

//
// MessageId: MSG_2221
//
// MessageText:
//
// SBIE2221 To add the program to Internet Access Restrictions, please double-click on this message line
//
#define MSG_2221                         0x810208ADL

//
// MessageId: MSG_2222
//
// MessageText:
//
// SBIE2222 To add the program to Start/Run Access Restrictions, please double-click on this message line
//
#define MSG_2222                         0x810208AEL

//
// MessageId: MSG_2223
//
// MessageText:
//
// SBIE2223 To increase the file size limit for copying files, please double-click on this message line
//
#define MSG_2223                         0x810208AFL

//
// MessageId: MSG_2224
//
// MessageText:
//
// SBIE2224 Sandboxed program has crashed: %2
//
#define MSG_2224                         0x810208B0L

//
// MessageId: MSG_2225
//
// MessageText:
//
// SBIE2225 An attempt was made to access an EFS file: %2
//
#define MSG_2225                         0x810208B1L

//
// MessageId: MSG_2226
//
// MessageText:
//
// SBIE2226 Process failed to start due to missing elevation, to resolve add "ApplyElevateCreateProcessFix=y" to the ini section for this box %2
//
#define MSG_2226                         0x810208B2L

//
// MessageId: MSG_2227
//
// MessageText:
//
// SBIE2227 '%2' is located on a volume which does not support 8.3 naming. This can cause issues with older applications and installers.
//
#define MSG_2227                         0x810208B3L

//
// MessageId: MSG_2230
//
// MessageText:
//
// SBIE2230 Failed to mount root for %2
//
#define MSG_2230                         0x810208B6L

//
// MessageId: MSG_2231
//
// MessageText:
//
// SBIE2231 Junction Target mismatch %2
//
#define MSG_2231                         0x810208B7L

//
// MessageId: MSG_2232
//
// MessageText:
//
// SBIE2232 The ImDisk Driver is not loaded
//
#define MSG_2232                         0x810208B8L

//
// MessageId: MSG_2233
//
// MessageText:
//
// SBIE2233 Cannot control the ImDisk Driver: %2
//
#define MSG_2233                         0x810208B9L

//
// MessageId: MSG_2234
//
// MessageText:
//
// SBIE2234 No free Drive letter found for temporary mount
//
#define MSG_2234                         0x810208BAL

//
// MessageId: MSG_2235
//
// MessageText:
//
// SBIE2235 Error undefining temporary drive letter: "%2"
//
#define MSG_2235                         0x810208BBL

//
// MessageId: MSG_2236
//
// MessageText:
//
// SBIE2236 %2 could not be invoked (perhaps it is not installed?)
//
#define MSG_2236                         0x810208BCL

//
// MessageId: MSG_2237
//
// MessageText:
//
// SBIE2237 Failed to unmount root %2
//
#define MSG_2237                         0x810208BDL

//
// MessageId: MSG_2238
//
// MessageText:
//
// SBIE2238 Ram Disk size is not configured, or to small, set RamDiskSizeKb=1048576 (1GB in Kilobytes) in the [GlobalSettings] ini section.
//
#define MSG_2238                         0x810208BEL

//
// MessageId: MSG_2239
//
// MessageText:
//
// SBIE2239 The root folder of sandbox %2 must be empty in order to mount a volume to it.
//
#define MSG_2239                         0x810208BFL

//
// MessageId: MSG_2240
//
// MessageText:
//
// SBIE2240 Timeout when trying to mount ImDisk volume %2
//
#define MSG_2240                         0x810208C0L

//
// MessageId: MSG_2241
//
// MessageText:
//
// SBIE2241 Box image file %2 could not be opened
//
#define MSG_2241                         0x810208C1L

//
// MessageId: MSG_2242
//
// MessageText:
//
// SBIE2242 Failed to mount box image, The specified cipher is not supported
//
#define MSG_2242                         0x810208C2L

//
// MessageId: MSG_2243
//
// MessageText:
//
// SBIE2243 Failed to mount box image, Wrong password
//
#define MSG_2243                         0x810208C3L

//
// MessageId: MSG_2244
//
// MessageText:
//
// SBIE2244 Failed to mount box image, Password required
//
#define MSG_2244                         0x810208C4L

//
// MessageId: MSG_2246
//
// MessageText:
//
// SBIE2246 Failed to mount box image, ImBox error %2
//
#define MSG_2246                         0x810208C6L

//
// MessageId: MSG_2301
//
// MessageText:
//
// SBIE2301 %2
//
#define MSG_2301                         0xC10208FDL

//
// MessageId: MSG_2302
//
// MessageText:
//
// SBIE2302 Process image configuration conflict: %2
//
#define MSG_2302                         0xC10208FEL

//
// MessageId: MSG_2303
//
// MessageText:
//
// SBIE2303 Could not hook %2
//
#define MSG_2303                         0xC10208FFL

//
// MessageId: MSG_2304
//
// MessageText:
//
// SBIE2304 Initialization failed for process %2
//
#define MSG_2304                         0xC1020900L

//
// MessageId: MSG_2305
//
// MessageText:
//
// SBIE2305 Out of memory
//
#define MSG_2305                         0xC1020901L

//
// MessageId: MSG_2306
//
// MessageText:
//
// SBIE2306 Could not locate user directory:  %2
//
#define MSG_2306                         0xC1020902L

//
// MessageId: MSG_2307
//
// MessageText:
//
// SBIE2307 Could not map drive %2
//
#define MSG_2307                         0xC1020903L

//
// MessageId: MSG_2308
//
// MessageText:
//
// SBIE2308 Could not create object directory:  %2
//
#define MSG_2308                         0xC1020904L

//
// MessageId: MSG_2309
//
// MessageText:
//
// SBIE2309 Could not disable COM+/DCOM:  %2
//
#define MSG_2309                         0xC1020905L

//
// MessageId: MSG_2310
//
// MessageText:
//
// SBIE2310 Name buffer is approaching overflow (%2)
//
#define MSG_2310                         0xC1020906L

//
// MessageId: MSG_2311
//
// MessageText:
//
// SBIE2311 Could not disable recycle bin (BitBucket):  %2
//
#define MSG_2311                         0xC1020907L

//
// MessageId: MSG_2312
//
// MessageText:
//
// SBIE2312 Could not enable BrowseNewProcess setting:  %2
//
#define MSG_2312                         0xC1020908L

//
// MessageId: MSG_2313
//
// MessageText:
//
// SBIE2313 Could not execute %2
//
#define MSG_2313                         0xC1020909L

//
// MessageId: MSG_2314
//
// MessageText:
//
// SBIE2314 Cancelling process %2
//
#define MSG_2314                         0xC102090AL

//
// MessageId: MSG_2316
//
// MessageText:
//
// SBIE2316 Memory corrupted
//
#define MSG_2316                         0xC102090CL

//
// MessageId: MSG_2317
//
// MessageText:
//
// SBIE2317 Cannot initialize path list '%2'
//
#define MSG_2317                         0xC102090DL

//
// MessageId: MSG_2318
//
// MessageText:
//
// SBIE2318 DLL initialization failed for '%2'
//
#define MSG_2318                         0xC102090EL

//
// MessageId: MSG_2321
//
// MessageText:
//
// SBIE2321 Cannot manage device map:  %2
//
#define MSG_2321                         0xC1020911L

//
// MessageId: MSG_2322
//
// MessageText:
//
// SBIE2322 Cannot rewrite Sandboxie.ini:  %2
//
#define MSG_2322                         0xC1020912L

//
// MessageId: MSG_2323
//
// MessageText:
//
// SBIE2323 Cryptography error:  %2
//
#define MSG_2323                         0xC1020913L

//
// MessageId: MSG_2325
//
// MessageText:
//
// SBIE2325 Debug:  %2
//
#define MSG_2325                         0xC1020915L

//
// MessageId: MSG_2326
//
// MessageText:
//
// SBIE2326 Cannot prepare registry:  %2
//
#define MSG_2326                         0xC1020916L

//
// MessageId: MSG_2327
//
// MessageText:
//
// SBIE2327 Error in COM server:  %2
//
#define MSG_2327                         0xC1020917L

//
// MessageId: MSG_2328
//
// MessageText:
//
// SBIE2328 Failed to resolve chrome sandbox hook %2
//
#define MSG_2328                         0xC1020918L

//
// MessageId: MSG_2329
//
// MessageText:
//
// SBIE2329 Failed to find FFS sequence %2
//
#define MSG_2329                         0xC1020919L

//
// MessageId: MSG_2330
//
// MessageText:
//
// SBIE2330 Unspecified error when hooking %2
//
#define MSG_2330                         0xC102091AL

//
// MessageId: MSG_2331
//
// MessageText:
//
// SBIE2331 Service start failed:  %2
//
#define MSG_2331                         0xC102091BL

//
// MessageId: MSG_2332
//
// MessageText:
//
// SBIE2332 Cannot access file SbiePst.dat
//
#define MSG_2332                         0xC102091CL

//
// MessageId: MSG_2335
//
// MessageText:
//
// SBIE2335 Initialization failed for process %2
//
#define MSG_2335                         0xC102091FL

//
// MessageId: MSG_2360
//
// MessageText:
//
// SBIE2360 Failed to inject SOCKS5 proxy:  %2
//
#define MSG_2360                         0xC1020938L

//
// MessageId: MSG_2336
//
// MessageText:
//
// SBIE2336 Error in GUI server:  %2
//
#define MSG_2336                         0xC1020920L

//
// MessageId: MSG_2337
//
// MessageText:
//
// SBIE2337 Failed to start program:  %2
//
#define MSG_2337                         0xC1020921L

//
// MessageId: MSG_2338
//
// MessageText:
//
// SBIE2338 Encountered unsupported architecture in process:  %2
//
#define MSG_2338                         0xC1020922L

//
// MessageId: MSG_9101
//
// MessageText:
//
// SBIE9101 Insufficient system resources
//
#define MSG_9101                         0xC101238DL

//
// MessageId: MSG_9153
//
// MessageText:
//
// SBIE9153 Cannot start driver (SbieDrv)
//
#define MSG_9153                         0xC10123C1L

//
// MessageId: MSG_9154
//
// MessageText:
//
// SBIE9154 Driver (SbieDrv) and service (SbieSvc) have different version numbers
//
#define MSG_9154                         0xC10123C2L

//
// MessageId: MSG_9156
//
// MessageText:
//
// SBIE9156 Driver initialization not completed
//
#define MSG_9156                         0xC10123C4L

//
// MessageId: MSG_9234
//
// MessageText:
//
// SBIE9234 Service startup error %2
//
#define MSG_9234                         0xC1012412L

//
// MessageId: MSG_3001
//
// MessageText:
//
// &OK
//
#define MSG_3001                         0x00000BB9L

//
// MessageId: MSG_3002
//
// MessageText:
//
// &Cancel
//
#define MSG_3002                         0x00000BBAL

//
// MessageId: MSG_3003
//
// MessageText:
//
// &Browse...
//
#define MSG_3003                         0x00000BBBL

//
// MessageId: MSG_3004
//
// MessageText:
//
// &Close
//
#define MSG_3004                         0x00000BBCL

//
// MessageId: MSG_3005
//
// MessageText:
//
// Cu&t
//
#define MSG_3005                         0x00000BBDL

//
// MessageId: MSG_3051
//
// MessageText:
//
// Delete protection is enabled for the sandbox
//
#define MSG_3051                         0x00000BEBL

//
// MessageId: MSG_3052
//
// MessageText:
//
// Run Sandboxed
//
#define MSG_3052                         0x00000BECL

//
// MessageId: MSG_3101
//
// MessageText:
//
// Sandboxie Start
//
#define MSG_3101                         0x00000C1DL

//
// MessageId: MSG_3103
//
// MessageText:
//
// Type the name of a program or folder and Sandboxie will open it for you.
//
#define MSG_3103                         0x00000C1FL

//
// MessageId: MSG_3104
//
// MessageText:
//
// Type  .  (the point character) to explore your desktop with Sandboxie.
//
#define MSG_3104                         0x00000C20L

//
// MessageId: MSG_3105
//
// MessageText:
//
// Remove command from command history
//
#define MSG_3105                         0x00000C21L

//
// MessageId: MSG_3106
//
// MessageText:
//
// Select the sandbox in which to start the program or document.
//
#define MSG_3106                         0x00000C22L

//
// MessageId: MSG_3107
//
// MessageText:
//
// Type the name of a program or path to open the following file in the current sandbox:
//
#define MSG_3107                         0x00000C23L

//
// MessageId: MSG_3111
//
// MessageText:
//
//       Sandboxie Start Menu - %2  %0
//
#define MSG_3111                         0x00000C27L

//
// MessageId: MSG_3112
//
// MessageText:
//
// Desktop
//
#define MSG_3112                         0x00000C28L

//
// MessageId: MSG_3113
//
// MessageText:
//
// (explore folder)
//
#define MSG_3113                         0x00000C29L

//
// MessageId: MSG_3114
//
// MessageText:
//
// Programs
//
#define MSG_3114                         0x00000C2AL

//
// MessageId: MSG_3115
//
// MessageText:
//
// Yes
//
#define MSG_3115                         0x00000C2BL

//
// MessageId: MSG_3116
//
// MessageText:
//
// No
//
#define MSG_3116                         0x00000C2CL

//
// MessageId: MSG_3117
//
// MessageText:
//
// Cancel
//
#define MSG_3117                         0x00000C2DL

//
// MessageId: MSG_3198
//
// MessageText:
//
// Do you want to start a new program in the %2 sandbox?
// You received this message because you set AlertBeforeStart=y.
//
#define MSG_3198                         0x00000C7EL

//
// MessageId: MSG_3199
//
// MessageText:
//
// This startup request does not appear to be invoked by the SANDBOXIE component. Are you sure you want to run it? If this is your action, you can ignore it and choose yes.
//
#define MSG_3199                         0x00000C7FL

//
// MessageId: MSG_3202
//
// MessageText:
//
// Invalid command line parameter:  %2
//
#define MSG_3202                         0x00000C82L

//
// MessageId: MSG_3203
//
// MessageText:
//
// Usage: Start.exe <program name>
//
#define MSG_3203                         0x00000C83L

//
// MessageId: MSG_3204
//
// MessageText:
//
// Invalid box name parameter:  %2
//
#define MSG_3204                         0x00000C84L

//
// MessageId: MSG_3205
//
// MessageText:
//
// Could not invoke program:
// 
// %2
//
#define MSG_3205                         0x00000C85L

//
// MessageId: MSG_3206
//
// MessageText:
//
// System Error Code:
//
#define MSG_3206                         0x00000C86L

//
// MessageId: MSG_3209
//
// MessageText:
//
// Cannot find the executable for the default mail agent
//
#define MSG_3209                         0x00000C89L

//
// MessageId: MSG_3210
//
// MessageText:
//
// Could not start Sandboxie Control program
//
#define MSG_3210                         0x00000C8AL

//
// MessageId: MSG_3212
//
// MessageText:
//
// The Sandboxie driver (SbieDrv) is not available to sandbox programs.
// Make sure both the driver and Sandboxie service (SbieSvc) have started successfully.
//
#define MSG_3212                         0x00000C8CL

//
// MessageId: MSG_3213
//
// MessageText:
//
// Could not initialize Sandboxie COM services
//
#define MSG_3213                         0x00000C8DL

//
// MessageId: MSG_3214
//
// MessageText:
//
// Delete Sandbox %2:  %0
//
#define MSG_3214                         0x00000C8EL

//
// MessageId: MSG_3215
//
// MessageText:
//
// The object (file or folder) may be in use by another program.
// Close any applications or windows that may prevent the access.
//
#define MSG_3215                         0x00000C8FL

//
// MessageId: MSG_3216
//
// MessageText:
//
// Could not locate the sandbox folder
//
#define MSG_3216                         0x00000C90L

//
// MessageId: MSG_3217
//
// MessageText:
//
// Could not open the sandbox folder
//
#define MSG_3217                         0x00000C91L

//
// MessageId: MSG_3218
//
// MessageText:
//
// Could not open the folder containing the sandbox folder
//
#define MSG_3218                         0x00000C92L

//
// MessageId: MSG_3219
//
// MessageText:
//
// Could not move the sandbox folder out of the way
//
#define MSG_3219                         0x00000C93L

//
// MessageId: MSG_3220
//
// MessageText:
//
// Error renaming one of the long file names in the sandbox.
// The contents of the sandbox will not be deleted.
//
#define MSG_3220                         0x00000C94L

//
// MessageId: MSG_3221
//
// MessageText:
//
// Please terminate programs running in the sandbox before deleting its contents
//
#define MSG_3221                         0x00000C95L

//
// MessageId: MSG_3222
//
// MessageText:
//
// Delete command failed: %2
//
#define MSG_3222                         0x00000C96L

//
// MessageId: MSG_3241
//
// MessageText:
//
// Sandboxie is requesting Administrator privileges
// on behalf of a program running in a sandbox.
//
#define MSG_3241                         0x00000CA9L

//
// MessageId: MSG_3242
//
// MessageText:
//
// To accept or cancel the request, activate the
// User Account Control pop-up window.
//
#define MSG_3242                         0x00000CAAL

//
// MessageId: MSG_3243
//
// MessageText:
//
// Note:  The program will continue to run under the supervision
// of Sandboxie even if Administrator privileges are granted.
// However, granting Administrator rights may increase the risk
// of the application exploiting an unpatched Windows vulnerability
// or a vulnerable driver to escape the sandbox.
//
#define MSG_3243                         0x00000CABL

//
// MessageId: MSG_3244
//
// MessageText:
//
// Application %2 is requesting Administrator privileges in sandbox:
//
#define MSG_3244                         0x00000CACL

//
// MessageId: MSG_3245
//
// MessageText:
//
// You can allow this request (YES), in which case a UAC prompt may 
// appear depending on your system settings. You can deny Administrator 
// privileges but make the application believe it has them (NO), or you can 
// abort the startup (CANCEL). 
//
#define MSG_3245                         0x00000CADL

//
// MessageId: MSG_3246
//
// MessageText:
//
// Note:  The program will continue to run under the supervision of Sandboxie even if Administrator 
// privileges are granted. However, granting real Administrator rights may increase the risk of the 
// application exploiting an unpatched Windows vulnerability or a vulnerable driver to escape the 
// sandbox. Therefore, it is generally safer to grant only fake Administrator rights, 
// although this may cause some installers to fail.
//
#define MSG_3246                         0x00000CAEL

//
// MessageId: MSG_3251
//
// MessageText:
//
// Run Outside Sandbox
//
#define MSG_3251                         0x00000CB3L

//
// MessageId: MSG_3252
//
// MessageText:
//
// Start the program expressly outside the supervision of
// Sandboxie, even if the program is forced to run under
// Sandboxie.  This exemption from sandboxing also extends
// to any other programs that are started by this program.
// 
// You can also select this option by holding the Ctrl and
// Shift keys down when you click the Run Sandboxed command.
//
#define MSG_3252                         0x00000CB4L

//
// MessageId: MSG_3253
//
// MessageText:
//
// Are you sure you wish to run the program outside the sandbox?
//
#define MSG_3253                         0x00000CB5L

//
// MessageId: MSG_3254
//
// MessageText:
//
// To skip this sandbox selection window, you can hold the Ctrl
// key down when you click the Run Sandboxed command.
//
#define MSG_3254                         0x00000CB6L

//
// MessageId: MSG_3255
//
// MessageText:
//
// Option is disabled because the Drop Rights setting is enabled for this sandbox
//
#define MSG_3255                         0x00000CB7L

//
// MessageId: MSG_3256
//
// MessageText:
//
// Option is disabled because this program is already running with Administrator privileges
//
#define MSG_3256                         0x00000CB8L

//
// MessageId: MSG_3301
//
// MessageText:
//
// Sandboxie Control
//
#define MSG_3301                         0x00000CE5L

//
// MessageId: MSG_3302
//
// MessageText:
//
// Version %2
//
#define MSG_3302                         0x00000CE6L

//
// MessageId: MSG_3303
//
// MessageText:
//
//   -  Initializing
//
#define MSG_3303                         0x00000CE7L

//
// MessageId: MSG_3304
//
// MessageText:
//
// Version mismatch in Sandboxie:
// 
// Sandboxie Control: Version %2
// Service (SbieSvc): Version %3
// Driver (SbieDrv): Version %4
// 
// Reinstall Sandboxie to resolve this problem.
// 
// Aborting.
//
#define MSG_3304                         0x00000CE8L

//
// MessageId: MSG_3306
//
// MessageText:
//
// In the future, don't show this message
//
#define MSG_3306                         0x00000CEAL

//
// MessageId: MSG_3308
//
// MessageText:
//
// Programs#*.EXE#All Files#*.*##
//
#define MSG_3308                         0x00000CECL

//
// MessageId: MSG_3309
//
// MessageText:
//
//   -  Deleting Sandbox
//
#define MSG_3309                         0x00000CEDL

//
// MessageId: MSG_3311
//
// MessageText:
//
// Failed to record configuration setting %3 in section %2:  %4
//
#define MSG_3311                         0x00000CEFL

//
// MessageId: MSG_3312
//
// MessageText:
//
// You are not authorized to update configuration in section %2
//
#define MSG_3312                         0x00000CF0L

//
// MessageId: MSG_3313
//
// MessageText:
//
// Unknown user, configuration settings will not be saved
//
#define MSG_3313                         0x00000CF1L

//
// MessageId: MSG_3314
//
// MessageText:
//
// An error occurred while applying default settings for sandbox '%2'
//
#define MSG_3314                         0x00000CF2L

//
// MessageId: MSG_3315
//
// MessageText:
//
// Deleting Sandbox contents
//
#define MSG_3315                         0x00000CF3L

//
// MessageId: MSG_3316
//
// MessageText:
//
// Do you want to abort the operation?
//
#define MSG_3316                         0x00000CF4L

//
// MessageId: MSG_3317
//
// MessageText:
//
// Preparing to delete: %2
//
#define MSG_3317                         0x00000CF5L

//
// MessageId: MSG_3318
//
// MessageText:
//
// Deleting: %2
//
#define MSG_3318                         0x00000CF6L

//
// MessageId: MSG_3351
//
// MessageText:
//
// &Remove
//
#define MSG_3351                         0x00000D17L

//
// MessageId: MSG_3352
//
// MessageText:
//
// &Add
//
#define MSG_3352                         0x00000D18L

//
// MessageId: MSG_3353
//
// MessageText:
//
// &Edit/Add
//
#define MSG_3353                         0x00000D19L

//
// MessageId: MSG_3354
//
// MessageText:
//
// &Edit
//
#define MSG_3354                         0x00000D1AL

//
// MessageId: MSG_3355
//
// MessageText:
//
// Add &Program
//
#define MSG_3355                         0x00000D1BL

//
// MessageId: MSG_3358
//
// MessageText:
//
// Add &Folder
//
#define MSG_3358                         0x00000D1EL

//
// MessageId: MSG_3359
//
// MessageText:
//
// Add &User
//
#define MSG_3359                         0x00000D1FL

//
// MessageId: MSG_3360
//
// MessageText:
//
// Select &All
//
#define MSG_3360                         0x00000D20L

//
// MessageId: MSG_3411
//
// MessageText:
//
// &File
//
#define MSG_3411                         0x00000D53L

//
// MessageId: MSG_3412
//
// MessageText:
//
// Terminate &All Programs
//
#define MSG_3412                         0x00000D54L

//
// MessageId: MSG_3413
//
// MessageText:
//
// Disable &Forced Programs
//
#define MSG_3413                         0x00000D55L

//
// MessageId: MSG_3414
//
// MessageText:
//
// Run As &UAC Administrator
//
#define MSG_3414                         0x00000D56L

//
// MessageId: MSG_3415
//
// MessageText:
//
// Is &Window Sandboxed?
//
#define MSG_3415                         0x00000D57L

//
// MessageId: MSG_3416
//
// MessageText:
//
// Resource Access &Monitor
//
#define MSG_3416                         0x00000D58L

//
// MessageId: MSG_3417
//
// MessageText:
//
// E&xit
//
#define MSG_3417                         0x00000D59L

//
// MessageId: MSG_3421
//
// MessageText:
//
// &View
//
#define MSG_3421                         0x00000D5DL

//
// MessageId: MSG_3422
//
// MessageText:
//
// &Programs
//
#define MSG_3422                         0x00000D5EL

//
// MessageId: MSG_3423
//
// MessageText:
//
// &Files and Folders
//
#define MSG_3423                         0x00000D5FL

//
// MessageId: MSG_3424
//
// MessageText:
//
// &Context Menu%2Shift+F10
//
#define MSG_3424                         0x00000D60L

//
// MessageId: MSG_3425
//
// MessageText:
//
// Recovery &Log
//
#define MSG_3425                         0x00000D61L

//
// MessageId: MSG_3426
//
// MessageText:
//
// &Always on Top
//
#define MSG_3426                         0x00000D62L

//
// MessageId: MSG_3431
//
// MessageText:
//
// Sand&box
//
#define MSG_3431                         0x00000D67L

//
// MessageId: MSG_3432
//
// MessageText:
//
// Create New Sandbox
//
#define MSG_3432                         0x00000D68L

//
// MessageId: MSG_3433
//
// MessageText:
//
// Set Container Folder
//
#define MSG_3433                         0x00000D69L

//
// MessageId: MSG_3434
//
// MessageText:
//
// Reveal Hidden Sandbox
//
#define MSG_3434                         0x00000D6AL

//
// MessageId: MSG_3435
//
// MessageText:
//
// Set Layout and Groups
//
#define MSG_3435                         0x00000D6BL

//
// MessageId: MSG_3441
//
// MessageText:
//
// &Configure
//
#define MSG_3441                         0x00000D71L

//
// MessageId: MSG_3442
//
// MessageText:
//
// &Program Alerts
//
#define MSG_3442                         0x00000D72L

//
// MessageId: MSG_3443
//
// MessageText:
//
// Windows Shell &Integration
//
#define MSG_3443                         0x00000D73L

//
// MessageId: MSG_3501
//
// MessageText:
//
// Software &Compatibility
//
#define MSG_3501                         0x00000DADL

//
// MessageId: MSG_3444
//
// MessageText:
//
// Forget Hidden &Messages
//
#define MSG_3444                         0x00000D74L

//
// MessageId: MSG_3445
//
// MessageText:
//
// &Tips
//
#define MSG_3445                         0x00000D75L

//
// MessageId: MSG_3502
//
// MessageText:
//
// Loc&k Configuration
//
#define MSG_3502                         0x00000DAEL

//
// MessageId: MSG_3446
//
// MessageText:
//
// &Edit Configuration
//
#define MSG_3446                         0x00000D76L

//
// MessageId: MSG_3447
//
// MessageText:
//
// Re&load Configuration
//
#define MSG_3447                         0x00000D77L

//
// MessageId: MSG_3448
//
// MessageText:
//
// &Show All Tips
//
#define MSG_3448                         0x00000D78L

//
// MessageId: MSG_3449
//
// MessageText:
//
// &Hide All Tips
//
#define MSG_3449                         0x00000D79L

//
// MessageId: MSG_3451
//
// MessageText:
//
// &Help
//
#define MSG_3451                         0x00000D7BL

//
// MessageId: MSG_3452
//
// MessageText:
//
// &Help Topics (Web)
//
#define MSG_3452                         0x00000D7CL

//
// MessageId: MSG_3453
//
// MessageText:
//
// Getting Started &Tutorial
//
#define MSG_3453                         0x00000D7DL

//
// MessageId: MSG_3454
//
// MessageText:
//
// Check for &Updates
//
#define MSG_3454                         0x00000D7EL

//
// MessageId: MSG_3456
//
// MessageText:
//
// &About Sandboxie
//
#define MSG_3456                         0x00000D80L

//
// MessageId: MSG_3457
//
// MessageText:
//
// &Sandboxie Forum (Web)
//
#define MSG_3457                         0x00000D81L

//
// MessageId: MSG_3459
//
// MessageText:
//
// Allow direct access to qWave driver (Google Hangouts)
//
#define MSG_3459                         0x00000D83L

//
// MessageId: MSG_3460
//
// MessageText:
//
// Function hooking customizations
//
#define MSG_3460                         0x00000D84L

//
// MessageId: MSG_3461
//
// MessageText:
//
// &Run Sandboxed
//
#define MSG_3461                         0x00000D85L

//
// MessageId: MSG_3462
//
// MessageText:
//
// Run Web &Browser
//
#define MSG_3462                         0x00000D86L

//
// MessageId: MSG_3463
//
// MessageText:
//
// Run Emai&l Reader
//
#define MSG_3463                         0x00000D87L

//
// MessageId: MSG_3464
//
// MessageText:
//
// Run &Any Program
//
#define MSG_3464                         0x00000D88L

//
// MessageId: MSG_3465
//
// MessageText:
//
// Run From Start &Menu
//
#define MSG_3465                         0x00000D89L

//
// MessageId: MSG_3466
//
// MessageText:
//
// Run &Windows Explorer
//
#define MSG_3466                         0x00000D8AL

//
// MessageId: MSG_3467
//
// MessageText:
//
// Upgrade to Sandboxie-Plus
//
#define MSG_3467                         0x00000D8BL

//
// MessageId: MSG_3468
//
// MessageText:
//
// Sandboxie-Plus Migration Guide
//
#define MSG_3468                         0x00000D8CL

//
// MessageId: MSG_3469
//
// MessageText:
//
// What's new in Sandboxie-Plus
//
#define MSG_3469                         0x00000D8DL

//
// MessageId: MSG_3471
//
// MessageText:
//
// &Terminate Programs
//
#define MSG_3471                         0x00000D8FL

//
// MessageId: MSG_3472
//
// MessageText:
//
// &Quick Recovery
//
#define MSG_3472                         0x00000D90L

//
// MessageId: MSG_3473
//
// MessageText:
//
// &Delete Contents
//
#define MSG_3473                         0x00000D91L

//
// MessageId: MSG_3474
//
// MessageText:
//
// &Explore Contents
//
#define MSG_3474                         0x00000D92L

//
// MessageId: MSG_3475
//
// MessageText:
//
// Sandbox &Settings
//
#define MSG_3475                         0x00000D93L

//
// MessageId: MSG_3476
//
// MessageText:
//
// Re&name Sandbox
//
#define MSG_3476                         0x00000D94L

//
// MessageId: MSG_3477
//
// MessageText:
//
// Remo&ve Sandbox
//
#define MSG_3477                         0x00000D95L

//
// MessageId: MSG_3481
//
// MessageText:
//
// &Terminate Program
//
#define MSG_3481                         0x00000D99L

//
// MessageId: MSG_3482
//
// MessageText:
//
// Program &Settings
//
#define MSG_3482                         0x00000D9AL

//
// MessageId: MSG_3483
//
// MessageText:
//
// &Resource Access
//
#define MSG_3483                         0x00000D9BL

//
// MessageId: MSG_3484
//
// MessageText:
//
// Resource Access
//
#define MSG_3484                         0x00000D9CL

//
// MessageId: MSG_3485
//
// MessageText:
//
// Show Window
//
#define MSG_3485                         0x00000D9DL

//
// MessageId: MSG_3486
//
// MessageText:
//
// Hide Window
//
#define MSG_3486                         0x00000D9EL

//
// MessageId: MSG_3487
//
// MessageText:
//
// &Show Errors
//
#define MSG_3487                         0x00000D9FL

//
// MessageId: MSG_3491
//
// MessageText:
//
// Reco&ver to Same Folder
//
#define MSG_3491                         0x00000DA3L

//
// MessageId: MSG_3492
//
// MessageText:
//
// Recover to &Any Folder
//
#define MSG_3492                         0x00000DA4L

//
// MessageId: MSG_3493
//
// MessageText:
//
// Ad&d Folder to Quick Recovery
//
#define MSG_3493                         0x00000DA5L

//
// MessageId: MSG_3494
//
// MessageText:
//
// Re&move Folder from Quick Recovery
//
#define MSG_3494                         0x00000DA6L

//
// MessageId: MSG_3495
//
// MessageText:
//
// Create Desktop Shortcut
//
#define MSG_3495                         0x00000DA7L

//
// MessageId: MSG_3511
//
// MessageText:
//
// Sandboxie Control will now hide its window, but will
// go on running.  To show the window, double-click the
// yellow Sandboxie icon in the system notification area.
//
#define MSG_3511                         0x00000DB7L

//
// MessageId: MSG_3512
//
// MessageText:
//
// Edit changes into the Sandboxie.ini configuration file
// in the text editor window which was now opened.
// 
// The revised configuration will take effect only after
// the Reload Configuration function was invoked.
// 
// In most cases, the Reload Configuration function is
// invoked automatically as soon as you close the text
// editor window.
// 
// You may also explicitly instruct Sandboxie Control to
// Reload Configuration at any time.
//
#define MSG_3512                         0x00000DB8L

//
// MessageId: MSG_3513
//
// MessageText:
//
// Sandboxie configuration has been refreshed and
// will apply to sandboxed programs as they start.
// 
// If any programs are already running sandboxed,
// the new configuration does not apply to them.
//
#define MSG_3513                         0x00000DB9L

//
// MessageId: MSG_3514
//
// MessageText:
//
// There was a problem reading the configuration file.
// Sandboxie configuration has not been refreshed.
//
#define MSG_3514                         0x00000DBAL

//
// MessageId: MSG_3515
//
// MessageText:
//
// Sandbox %2
//
#define MSG_3515                         0x00000DBBL

//
// MessageId: MSG_3516
//
// MessageText:
//
// Active
//
#define MSG_3516                         0x00000DBCL

//
// MessageId: MSG_3517
//
// MessageText:
//
// Program Name
//
#define MSG_3517                         0x00000DBDL

//
// MessageId: MSG_3518
//
// MessageText:
//
// PID
//
#define MSG_3518                         0x00000DBEL

//
// MessageId: MSG_3519
//
// MessageText:
//
// Window Title
//
#define MSG_3519                         0x00000DBFL

//
// MessageId: MSG_3520
//
// MessageText:
//
// Quick Recovery Folders
//
#define MSG_3520                         0x00000DC0L

//
// MessageId: MSG_3521
//
// MessageText:
//
// All Files and Folders
//
#define MSG_3521                         0x00000DC1L

//
// MessageId: MSG_3522
//
// MessageText:
//
// Drives
//
#define MSG_3522                         0x00000DC2L

//
// MessageId: MSG_3523
//
// MessageText:
//
// Network Shares
//
#define MSG_3523                         0x00000DC3L

//
// MessageId: MSG_3524
//
// MessageText:
//
// User Files
//
#define MSG_3524                         0x00000DC4L

//
// MessageId: MSG_3525
//
// MessageText:
//
// Personal
//
#define MSG_3525                         0x00000DC5L

//
// MessageId: MSG_3526
//
// MessageText:
//
// All Users
//
#define MSG_3526                         0x00000DC6L

//
// MessageId: MSG_3527
//
// MessageText:
//
// Please delete the contents of sandbox %2 before invoking this command.
//
#define MSG_3527                         0x00000DC7L

//
// MessageId: MSG_3528
//
// MessageText:
//
// Enter a new name for the sandbox.
//
#define MSG_3528                         0x00000DC8L

//
// MessageId: MSG_3529
//
// MessageText:
//
// Are you sure you want to remove sandbox %2?
//
#define MSG_3529                         0x00000DC9L

//
// MessageId: MSG_3530
//
// MessageText:
//
// There are no processes to terminate.
//
#define MSG_3530                         0x00000DCAL

//
// MessageId: MSG_3531
//
// MessageText:
//
// WARNING: The process or processes to be terminated
// will not be given the chance to save state or data.
// 
// Are you sure you want to proceed with process termination?
//
#define MSG_3531                         0x00000DCBL

//
// MessageId: MSG_3532
//
// MessageText:
//
// In the future, terminate processes without asking
//
#define MSG_3532                         0x00000DCCL

//
// MessageId: MSG_3533
//
// MessageText:
//
// You can copy and cut files and folders from a sandboxed Windows Explorer
// folder window, and paste them directly into an unsandboxed folder window.
//
#define MSG_3533                         0x00000DCDL

//
// MessageId: MSG_3534
//
// MessageText:
//
// You will be exploring the contents of the sandbox using a Windows Explorer
// window that is not running under the supervision of Sandboxie.
// 
// You may open programs or documents that reside within the sandbox.
// The program or document will start under the supervision of Sandboxie.
// 
// However, be advised to manipulate the contents of the sandbox using
// an instance of Windows Explorer running under Sandboxie.  To do this, use
// the command:  Sandbox > Run Sandboxed > Run Windows Explorer.
//
#define MSG_3534                         0x00000DCEL

//
// MessageId: MSG_3535
//
// MessageText:
//
// An error occurred while accessing the sandbox folder.
//
#define MSG_3535                         0x00000DCFL

//
// MessageId: MSG_3536
//
// MessageText:
//
// The 'hidden' indicator has been removed from all messages.
//
#define MSG_3536                         0x00000DD0L

//
// MessageId: MSG_3537
//
// MessageText:
//
// The 'don't show this message' indicator has been removed from all tips.
//
#define MSG_3537                         0x00000DD1L

//
// MessageId: MSG_3538
//
// MessageText:
//
// The 'don't show this message' indicator now applies to all tips.
//
#define MSG_3538                         0x00000DD2L

//
// MessageId: MSG_3539
//
// MessageText:
//
// Sandbox '%2' was created %3 days ago.
// 
// Please consider deleting the contents of your sandbox from time to time.
// 
// To configure automatic deletion, use the Sandbox Settings command from
// the Sandbox menu in Sandboxie Control.
//
#define MSG_3539                         0x00000DD3L

//
// MessageId: MSG_3540
//
// MessageText:
//
// The Sandboxie.ini configuration file cannot be edited because
// it is password-protected.  To edit the file, use the
// Lock Configuration command to disable password protection.
//
#define MSG_3540                         0x00000DD4L

//
// MessageId: MSG_3541
//
// MessageText:
//
// Public
//
#define MSG_3541                         0x00000DD5L

//
// MessageId: MSG_3601
//
// MessageText:
//
// About Sandboxie
//
#define MSG_3601                         0x00000E11L

//
// MessageId: MSG_3621
//
// MessageText:
//
// Check For Updates
//
#define MSG_3621                         0x00000E25L

//
// MessageId: MSG_3622
//
// MessageText:
//
// Connect to the Sandboxie Web site to check if a newer version of the program is available?
//
#define MSG_3622                         0x00000E26L

//
// MessageId: MSG_3623
//
// MessageText:
//
// &Now
//
#define MSG_3623                         0x00000E27L

//
// MessageId: MSG_3624
//
// MessageText:
//
// Next &Week
//
#define MSG_3624                         0x00000E28L

//
// MessageId: MSG_3625
//
// MessageText:
//
// Ne&ver
//
#define MSG_3625                         0x00000E29L

//
// MessageId: MSG_3626
//
// MessageText:
//
// In the future, check for updates without asking
//
#define MSG_3626                         0x00000E2AL

//
// MessageId: MSG_3627
//
// MessageText:
//
// Privacy Policy:  Your personal information is neither transmitted nor collected by Sandboxie.
//
#define MSG_3627                         0x00000E2BL

//
// MessageId: MSG_3628
//
// MessageText:
//
// Update check is already in progress.  Please wait.
//
#define MSG_3628                         0x00000E2CL

//
// MessageId: MSG_3629
//
// MessageText:
//
// The Sandboxie Web site does not report a newer version of Sandboxie.
//
#define MSG_3629                         0x00000E2DL

//
// MessageId: MSG_3630
//
// MessageText:
//
// Sandboxie %2 is available for download.
//
#define MSG_3630                         0x00000E2EL

//
// MessageId: MSG_3631
//
// MessageText:
//
// Download now?
// 
// Note:  The download takes about a minute.
// You will be notified when the download completes.
//
#define MSG_3631                         0x00000E2FL

//
// MessageId: MSG_3632
//
// MessageText:
//
// To download and install the new version, log into a user account
// with administrative privileges, and invoke this command again.
//
#define MSG_3632                         0x00000E30L

//
// MessageId: MSG_3633
//
// MessageText:
//
// Sandboxie %2 has been downloaded to the following location:
// 
// %3
// 
// Click OK to begin the installation.  If any programs are
// running sandboxed, they will be terminated.
//
#define MSG_3633                         0x00000E31L

//
// MessageId: MSG_3634
//
// MessageText:
//
// Updated failed:  An error occurred while communicating with the Sandboxie Web site.
//
#define MSG_3634                         0x00000E32L

//
// MessageId: MSG_3635
//
// MessageText:
//
// downloading new version
//
#define MSG_3635                         0x00000E33L

//
// MessageId: MSG_3641
//
// MessageText:
//
// Open Web Browser
//
#define MSG_3641                         0x00000E39L

//
// MessageId: MSG_3642
//
// MessageText:
//
// Select whether to open the following Web address in a sandboxed or unsandboxed Web browser.
//
#define MSG_3642                         0x00000E3AL

//
// MessageId: MSG_3643
//
// MessageText:
//
// &Sandboxed
//
#define MSG_3643                         0x00000E3BL

//
// MessageId: MSG_3644
//
// MessageText:
//
// &Unsandboxed
//
#define MSG_3644                         0x00000E3CL

//
// MessageId: MSG_3645
//
// MessageText:
//
// Messages from Sandboxie
//
#define MSG_3645                         0x00000E3DL

//
// MessageId: MSG_3646
//
// MessageText:
//
// H&ide
//
#define MSG_3646                         0x00000E3EL

//
// MessageId: MSG_3647
//
// MessageText:
//
// Hiding a message will prevent the message from being displayed
// in the future.  Hiding a message does not actually resolve the
// problems that have caused the message to be displayed.
// 
// Are you sure you want to turn off reporting for message %2?
//
#define MSG_3647                         0x00000E3FL

//
// MessageId: MSG_3651
//
// MessageText:
//
// Temporarily Disable Forced Programs
//
#define MSG_3651                         0x00000E43L

//
// MessageId: MSG_3652
//
// MessageText:
//
// Temporarily Disable Forced Programs for
//
#define MSG_3652                         0x00000E44L

//
// MessageId: MSG_3653
//
// MessageText:
//
// seconds
//
#define MSG_3653                         0x00000E45L

//
// MessageId: MSG_3654
//
// MessageText:
//
// When you click OK, the Forced Programs mechanism will be temporarily disabled, for a duration of the number of seconds specified above, or until you select this function again.
//
#define MSG_3654                         0x00000E46L

//
// MessageId: MSG_3599
//
// MessageText:
//
// You can disable forcing of a specific program by holding the Ctrl and Shift keys down when you click the Run Sandboxed command for that program.
//
#define MSG_3599                         0x00000E0FL

//
// MessageId: MSG_3655
//
// MessageText:
//
// Resource Access Monitor
//
#define MSG_3655                         0x00000E47L

//
// MessageId: MSG_3656
//
// MessageText:
//
// This tool monitors programs running under the supervision of Sandboxie, and displays the resources they access.  Please consult the documentation before using this tool.
//
#define MSG_3656                         0x00000E48L

//
// MessageId: MSG_3657
//
// MessageText:
//
// Copy Contents to Clipboard and Close Window
//
#define MSG_3657                         0x00000E49L

//
// MessageId: MSG_3661
//
// MessageText:
//
// Is Window Sandboxed?
//
#define MSG_3661                         0x00000E4DL

//
// MessageId: MSG_3662
//
// MessageText:
//
// Drag the Finder Tool over a window to select it, then release the mouse to check if the window is sandboxed.
// 
// Press ESC to cancel.
//
#define MSG_3662                         0x00000E4EL

//
// MessageId: MSG_3663
//
// MessageText:
//
// The selected window is running as part of program %2 in sandbox %3.
//
#define MSG_3663                         0x00000E4FL

//
// MessageId: MSG_3664
//
// MessageText:
//
// The selected window is not running as part of any sandboxed program.
//
#define MSG_3664                         0x00000E50L

//
// MessageId: MSG_3665
//
// MessageText:
//
// Create a New Sandbox
//
#define MSG_3665                         0x00000E51L

//
// MessageId: MSG_3666
//
// MessageText:
//
// Enter a name for the new sandbox.
//
#define MSG_3666                         0x00000E52L

//
// MessageId: MSG_3667
//
// MessageText:
//
// Error:  Invalid sandbox name entered.  Use only letters and digits.  Spaces and other special characters are not allowed.  The name cannot exceed 32 characters in length.
//
#define MSG_3667                         0x00000E53L

//
// MessageId: MSG_3668
//
// MessageText:
//
// Error:  Duplicate sandbox name entered.  Enter a name not already in use by another sandbox.
//
#define MSG_3668                         0x00000E54L

//
// MessageId: MSG_3669
//
// MessageText:
//
// Copy settings from existing sandbox:
//
#define MSG_3669                         0x00000E55L

//
// MessageId: MSG_4665
//
// MessageText:
//
// Error:  Duplicate sandbox name entered.  The name is already used by a hidden sandbox.  Use the "Reveal Hidden Sandbox" command from the Sandbox menu.
//
#define MSG_4665                         0x00001239L

//
// MessageId: MSG_3671
//
// MessageText:
//
// Set Container Folder
//
#define MSG_3671                         0x00000E57L

//
// MessageId: MSG_3672
//
// MessageText:
//
// Select the drive in which sandboxes will be created and stored.
//
#define MSG_3672                         0x00000E58L

//
// MessageId: MSG_3673
//
// MessageText:
//
// For maximum compatibility with other programs, it is recommended that you leave the sandbox at the default location in the drive you select.
//
#define MSG_3673                         0x00000E59L

//
// MessageId: MSG_3674
//
// MessageText:
//
// However, you may also use the edit box below to override the default location:
//
#define MSG_3674                         0x00000E5AL

//
// MessageId: MSG_3675
//
// MessageText:
//
// Note:  You are advised to delete contents in all sandboxes, before changing the container folder.
//
#define MSG_3675                         0x00000E5BL

//
// MessageId: MSG_3676
//
// MessageText:
//
// Drive %2
//
#define MSG_3676                         0x00000E5CL

//
// MessageId: MSG_3681
//
// MessageText:
//
// Program Alerts
//
#define MSG_3681                         0x00000E61L

//
// MessageId: MSG_3682
//
// MessageText:
//
// When any of following programs is launched outside any sandbox, Sandboxie will issue message SBIE1301.
//
#define MSG_3682                         0x00000E62L

//
// MessageId: MSG_3685
//
// MessageText:
//
// Windows Shell Integration
//
#define MSG_3685                         0x00000E65L

//
// MessageId: MSG_3686
//
// MessageText:
//
// Start Sandboxie Control
//
#define MSG_3686                         0x00000E66L

//
// MessageId: MSG_3687
//
// MessageText:
//
// When Windows starts
//
#define MSG_3687                         0x00000E67L

//
// MessageId: MSG_3688
//
// MessageText:
//
// When a program starts in the sandbox
//
#define MSG_3688                         0x00000E68L

//
// MessageId: MSG_3689
//
// MessageText:
//
// Shortcut Icons
//
#define MSG_3689                         0x00000E69L

//
// MessageId: MSG_3690
//
// MessageText:
//
// Add desktop shortcut for starting Web browser under Sandboxie
//
#define MSG_3690                         0x00000E6AL

//
// MessageId: MSG_3691
//
// MessageText:
//
// Add Quick Launch shortcut for starting Web browser under Sandboxie
//
#define MSG_3691                         0x00000E6BL

//
// MessageId: MSG_3692
//
// MessageText:
//
// Click to add more desktop shortcut icons:
//
#define MSG_3692                         0x00000E6CL

//
// MessageId: MSG_3693
//
// MessageText:
//
// &Add Shortcut Icons
//
#define MSG_3693                         0x00000E6DL

//
// MessageId: MSG_3694
//
// MessageText:
//
// "Run Sandboxed" Actions
//
#define MSG_3694                         0x00000E6EL

//
// MessageId: MSG_3695
//
// MessageText:
//
// Add right-click action "Run Sandboxed" to files and folders
//
#define MSG_3695                         0x00000E6FL

//
// MessageId: MSG_3696
//
// MessageText:
//
// Add sandboxes as targets for "Send To" action
//
#define MSG_3696                         0x00000E70L

//
// MessageId: MSG_3697
//
// MessageText:
//
// The Sandboxie Start Menu will now be displayed.  Select an
// application from the menu, and Sandboxie will create a new
// shortcut icon on your real desktop, which you can use to
// invoke the selected application under the supervision of
// Sandboxie.
//
#define MSG_3697                         0x00000E71L

//
// MessageId: MSG_3698
//
// MessageText:
//
// Sandboxed Web Browser.lnk
//
#define MSG_3698                         0x00000E72L

//
// MessageId: MSG_3699
//
// MessageText:
//
// Run &Sandboxed
//
#define MSG_3699                         0x00000E73L

//
// MessageId: MSG_4491
//
// MessageText:
//
// Open %2 in sandbox %3
//
#define MSG_4491                         0x0000118BL

//
// MessageId: MSG_3711
//
// MessageText:
//
// Delete Contents|Quick Recovery
//
#define MSG_3711                         0x00000E7FL

//
// MessageId: MSG_3712
//
// MessageText:
//
// The files listed below are eligible for quick recovery from the sandbox.  Select a file or folder and click either of the Recover buttons below to recover the selected item.
//
#define MSG_3712                         0x00000E80L

//
// MessageId: MSG_3713
//
// MessageText:
//
// Click Delete Contents to terminate any processes running in the sandbox, and delete its contents.
//
#define MSG_3713                         0x00000E81L

//
// MessageId: MSG_3714
//
// MessageText:
//
// &Delete Contents
//
#define MSG_3714                         0x00000E82L

//
// MessageId: MSG_3715
//
// MessageText:
//
// Immediate Recovery
//
#define MSG_3715                         0x00000E83L

//
// MessageId: MSG_3716
//
// MessageText:
//
// The following files are eligible for immediate recovery and can be moved out of the sandbox.
//
#define MSG_3716                         0x00000E84L

//
// MessageId: MSG_3717
//
// MessageText:
//
// First select files from the list above, then select a destination folder.
//
#define MSG_3717                         0x00000E85L

//
// MessageId: MSG_3718
//
// MessageText:
//
// &Recover
//
#define MSG_3718                         0x00000E86L

//
// MessageId: MSG_3719
//
// MessageText:
//
// Don't &prompt again for immediate recovery until all programs in this sandbox have ended
//
#define MSG_3719                         0x00000E87L

//
// MessageId: MSG_3720
//
// MessageText:
//
// Reco&ver to
// Same Folder
//
#define MSG_3720                         0x00000E88L

//
// MessageId: MSG_3721
//
// MessageText:
//
// Recover to
// &Any Folder
//
#define MSG_3721                         0x00000E89L

//
// MessageId: MSG_3722
//
// MessageText:
//
// There are %2 files and %3 folders in the sandbox, occupying %4 bytes of disk space.
//
#define MSG_3722                         0x00000E8AL

//
// MessageId: MSG_3723
//
// MessageText:
//
// Select a folder into which the file or folder will be recovered.
//
#define MSG_3723                         0x00000E8BL

//
// MessageId: MSG_3724
//
// MessageText:
//
// Or select a folder from an earlier recovery.
//
#define MSG_3724                         0x00000E8CL

//
// MessageId: MSG_3725
//
// MessageText:
//
// Selected folders will be stored for later use
//
#define MSG_3725                         0x00000E8DL

//
// MessageId: MSG_3726
//
// MessageText:
//
// Select a folder to add to the list of Quick Recovery folders.
//
#define MSG_3726                         0x00000E8EL

//
// MessageId: MSG_3727
//
// MessageText:
//
// The folder was added to the list of Quick Recovery folders.
// 
// However, at this time, there are no sandboxed items that can
// be recovered from the folder.
//
#define MSG_3727                         0x00000E8FL

//
// MessageId: MSG_3728
//
// MessageText:
//
// No files have been selected for recovery.
//
#define MSG_3728                         0x00000E90L

//
// MessageId: MSG_3729
//
// MessageText:
//
// Recovery failed.  The following item could not be moved out of the sandbox:
//
#define MSG_3729                         0x00000E91L

//
// MessageId: MSG_3730
//
// MessageText:
//
// The sandbox is empty.  There is nothing to delete.
//
#define MSG_3730                         0x00000E92L

//
// MessageId: MSG_3731
//
// MessageText:
//
// Some sandboxed programs could not be stopped.  The contents of the sandbox will not be deleted.
//
#define MSG_3731                         0x00000E93L

//
// MessageId: MSG_3732
//
// MessageText:
//
// There are no files to recover at this time.
//
#define MSG_3732                         0x00000E94L

//
// MessageId: MSG_3733
//
// MessageText:
//
// File cannot be recovered because the name of the destination folder is too long.
// Please select some other destination folder.
//
#define MSG_3733                         0x00000E95L

//
// MessageId: MSG_3734
//
// MessageText:
//
// Right-click the list of folders for more options
//
#define MSG_3734                         0x00000E96L

//
// MessageId: MSG_3735
//
// MessageText:
//
// Replace all files without additional prompts
//
#define MSG_3735                         0x00000E97L

//
// MessageId: MSG_3736
//
// MessageText:
//
// &Recover && Explore
//
#define MSG_3736                         0x00000E98L

//
// MessageId: MSG_3737
//
// MessageText:
//
// &Recover && Run
//
#define MSG_3737                         0x00000E99L

//
// MessageId: MSG_3981
//
// MessageText:
//
// &Open folder in Windows Explorer outside the sandbox
//
#define MSG_3981                         0x00000F8DL

//
// MessageId: MSG_3982
//
// MessageText:
//
// Remove this folder from the list
//
#define MSG_3982                         0x00000F8EL

//
// MessageId: MSG_3983
//
// MessageText:
//
// Remove all folders from the list
//
#define MSG_3983                         0x00000F8FL

//
// MessageId: MSG_3986
//
// MessageText:
//
// Recovery Log
//
#define MSG_3986                         0x00000F92L

//
// MessageId: MSG_3987
//
// MessageText:
//
// The following files were recently recovered and moved out of sandboxes.
//
#define MSG_3987                         0x00000F93L

//
// MessageId: MSG_3741
//
// MessageText:
//
// Program Settings
//
#define MSG_3741                         0x00000E9DL

//
// MessageId: MSG_3742
//
// MessageText:
//
// Sandbox:
//
#define MSG_3742                         0x00000E9EL

//
// MessageId: MSG_3743
//
// MessageText:
//
// Program:
//
#define MSG_3743                         0x00000E9FL

//
// MessageId: MSG_3744
//
// MessageText:
//
// View Page 1
//
#define MSG_3744                         0x00000EA0L

//
// MessageId: MSG_3745
//
// MessageText:
//
// View Page 2
//
#define MSG_3745                         0x00000EA1L

//
// MessageId: MSG_3751
//
// MessageText:
//
// Program Start
//
#define MSG_3751                         0x00000EA7L

//
// MessageId: MSG_3752
//
// MessageText:
//
// Whenever this program starts outside any sandbox:
//
#define MSG_3752                         0x00000EA8L

//
// MessageId: MSG_3753
//
// MessageText:
//
// Issue alert message SBIE1301
//
#define MSG_3753                         0x00000EA9L

//
// MessageId: MSG_3754
//
// MessageText:
//
// Force program to run in this sandbox
//
#define MSG_3754                         0x00000EAAL

//
// MessageId: MSG_3755
//
// MessageText:
//
// Program Stop
//
#define MSG_3755                         0x00000EABL

//
// MessageId: MSG_3756
//
// MessageText:
//
// Stop this program if it lingers in the sandbox after other programs have ended
//
#define MSG_3756                         0x00000EACL

//
// MessageId: MSG_3757
//
// MessageText:
//
// Stop other programs after this leader program has ended
//
#define MSG_3757                         0x00000EADL

//
// MessageId: MSG_3761
//
// MessageText:
//
// Internet Restrictions
//
#define MSG_3761                         0x00000EB1L

//
// MessageId: MSG_3762
//
// MessageText:
//
// Any program running in this sandbox can connect to the Internet.
//
#define MSG_3762                         0x00000EB2L

//
// MessageId: MSG_3763
//
// MessageText:
//
// Programs that can connect to the Internet in this sandbox:
//
#define MSG_3763                         0x00000EB3L

//
// MessageId: MSG_3764
//
// MessageText:
//
// Enable restrictions and allow this program to connect to the Internet.
//
#define MSG_3764                         0x00000EB4L

//
// MessageId: MSG_3765
//
// MessageText:
//
// Start/Run Restrictions
//
#define MSG_3765                         0x00000EB5L

//
// MessageId: MSG_3766
//
// MessageText:
//
// Any program can be started in this sandbox.
//
#define MSG_3766                         0x00000EB6L

//
// MessageId: MSG_3767
//
// MessageText:
//
// Programs that can be started in this sandbox:
//
#define MSG_3767                         0x00000EB7L

//
// MessageId: MSG_3768
//
// MessageText:
//
// Enable restrictions and allow this program to start.
//
#define MSG_3768                         0x00000EB8L

//
// MessageId: MSG_3769
//
// MessageText:
//
// (None)
//
#define MSG_3769                         0x00000EB9L

//
// MessageId: MSG_3770
//
// MessageText:
//
// The new settings do not apply to the sandboxed
// program that is already running.  The settings
// will apply the next time this program starts.
//
#define MSG_3770                         0x00000EBAL

//
// MessageId: MSG_3772
//
// MessageText:
//
// Are you sure you want to set %2 as a forced program?
//
#define MSG_3772                         0x00000EBCL

//
// MessageId: MSG_3773
//
// MessageText:
//
// Are you sure you want to set %2 as a forced folder?
//
#define MSG_3773                         0x00000EBDL

//
// MessageId: MSG_3775
//
// MessageText:
//
// You have made a change that may not be easy to undo.
//
#define MSG_3775                         0x00000EBFL

//
// MessageId: MSG_3801
//
// MessageText:
//
// Sandbox Settings - %2
//
#define MSG_3801                         0x00000ED9L

//
// MessageId: MSG_3802
//
// MessageText:
//
// This page intentionally left blank
//
#define MSG_3802                         0x00000EDAL

//
// MessageId: MSG_3803
//
// MessageText:
//
// You must apply the changes in this page before you can proceed.
//
#define MSG_3803                         0x00000EDBL

//
// MessageId: MSG_3804
//
// MessageText:
//
// Apply changes when switching to another page.
//
#define MSG_3804                         0x00000EDCL

//
// MessageId: MSG_3805
//
// MessageText:
//
// All Programs
//
#define MSG_3805                         0x00000EDDL

//
// MessageId: MSG_3806
//
// MessageText:
//
// The list below applies to
//
#define MSG_3806                         0x00000EDEL

//
// MessageId: MSG_3807
//
// MessageText:
//
// Select a file or folder to add to the list of File Access exclusions.
//
#define MSG_3807                         0x00000EDFL

//
// MessageId: MSG_3808
//
// MessageText:
//
// Add Resource Name
//
#define MSG_3808                         0x00000EE0L

//
// MessageId: MSG_3809
//
// MessageText:
//
// Edit Resource Name
//
#define MSG_3809                         0x00000EE1L

//
// MessageId: MSG_3551
//
// MessageText:
//
// (All programs except %2)
//
#define MSG_3551                         0x00000DDFL

//
// MessageId: MSG_3552
//
// MessageText:
//
// Reverse the meaning of the current selection
//
#define MSG_3552                         0x00000DE0L

//
// MessageId: MSG_3811
//
// MessageText:
//
// Appearance
//
#define MSG_3811                         0x00000EE3L

//
// MessageId: MSG_3812
//
// MessageText:
//
// Sandboxed programs running under the supervision of Sandboxie will show the Sandboxie [#] indicator in the window title.
//
#define MSG_3812                         0x00000EE4L

//
// MessageId: MSG_3813
//
// MessageText:
//
// If the following setting is enabled, the window title will also display the name of this sandbox.
//
#define MSG_3813                         0x00000EE5L

//
// MessageId: MSG_3814
//
// MessageText:
//
// Show sandbox name in window title
//
#define MSG_3814                         0x00000EE6L

//
// MessageId: MSG_3815
//
// MessageText:
//
// Don't show Sandboxie indicator in the window title
//
#define MSG_3815                         0x00000EE7L

//
// MessageId: MSG_3816
//
// MessageText:
//
// Sandboxie can display a thick border around a window which belongs to a sandboxed program.
//
#define MSG_3816                         0x00000EE8L

//
// MessageId: MSG_3817
//
// MessageText:
//
// Display a border around the window
//
#define MSG_3817                         0x00000EE9L

//
// MessageId: MSG_3818
//
// MessageText:
//
// Click to change border colour
//
#define MSG_3818                         0x00000EEAL

//
// MessageId: MSG_3819
//
// MessageText:
//
// Display the border only when the mouse cursor is in the window title
//
#define MSG_3819                         0x00000EEBL

//
// MessageId: MSG_3820
//
// MessageText:
//
// Alpha
//
#define MSG_3820                         0x00000EECL

//
// MessageId: MSG_3821
//
// MessageText:
//
// Recovery
//
#define MSG_3821                         0x00000EEDL

//
// MessageId: MSG_3822
//
// MessageText:
//
// Quick Recovery
//
#define MSG_3822                         0x00000EEEL

//
// MessageId: MSG_3823
//
// MessageText:
//
// Before the contents of the sandbox are deleted, or when you manually invoke the Quick Recovery function, the following folders will be checked for sandboxed content.  If any files are found, you will get a chance to easily recover them out of the sandbox.
//
#define MSG_3823                         0x00000EEFL

//
// MessageId: MSG_3832
//
// MessageText:
//
// Immediate Recovery enhances Quick Recovery by automatically invoking the recovery function as soon as files are created.
//
#define MSG_3832                         0x00000EF8L

//
// MessageId: MSG_3833
//
// MessageText:
//
// Enable Immediate Recovery
//
#define MSG_3833                         0x00000EF9L

//
// MessageId: MSG_3834
//
// MessageText:
//
// The following folders and file types (or file extensions) will be excluded from Immediate Recovery.
//
#define MSG_3834                         0x00000EFAL

//
// MessageId: MSG_3835
//
// MessageText:
//
// Add &Type
//
#define MSG_3835                         0x00000EFBL

//
// MessageId: MSG_3836
//
// MessageText:
//
// Select a folder to exclude from Immediate Recovery.
//
#define MSG_3836                         0x00000EFCL

//
// MessageId: MSG_3837
//
// MessageText:
//
// Enter a file type (or file extension) to exclude from Immediate Recovery
//
#define MSG_3837                         0x00000EFDL

//
// MessageId: MSG_3841
//
// MessageText:
//
// Delete
//
#define MSG_3841                         0x00000F01L

//
// MessageId: MSG_3842
//
// MessageText:
//
// Delete Invocation
//
#define MSG_3842                         0x00000F02L

//
// MessageId: MSG_3843
//
// MessageText:
//
// The contents of this sandbox can be automatically deleted when the last sandboxed program ends and the sandbox becomes inactive.
//
#define MSG_3843                         0x00000F03L

//
// MessageId: MSG_3844
//
// MessageText:
//
// Automatically delete contents of sandbox
//
#define MSG_3844                         0x00000F04L

//
// MessageId: MSG_3845
//
// MessageText:
//
// If any sandboxed files are eligible for recovery, Quick Recovery will be invoked instead of automatic delete.
//
#define MSG_3845                         0x00000F05L

//
// MessageId: MSG_3846
//
// MessageText:
//
// Alternatively, this sandbox can be protected from removal and deletion, both manual and automatic, that are initiated by Sandboxie.
//
#define MSG_3846                         0x00000F06L

//
// MessageId: MSG_3847
//
// MessageText:
//
// Never remove this sandbox or delete its contents
//
#define MSG_3847                         0x00000F07L

//
// MessageId: MSG_3852
//
// MessageText:
//
// Delete Command
//
#define MSG_3852                         0x00000F0CL

//
// MessageId: MSG_3853
//
// MessageText:
//
// For both automatic and manual deletion, the following system command will be used to delete the contents of the sandbox.  If left blank, the default command is RMDIR (remove directory).
//
#define MSG_3853                         0x00000F0DL

//
// MessageId: MSG_3854
//
// MessageText:
//
// If you change this setting, include the text "%%SANDBOX%%" (with quote marks).  When the command is executed, this text will be replaced with the location of the sandbox folder.
//
#define MSG_3854                         0x00000F0EL

//
// MessageId: MSG_3855
//
// MessageText:
//
// Or select a preset delete command:
//
#define MSG_3855                         0x00000F0FL

//
// MessageId: MSG_3856
//
// MessageText:
//
// The delete command does not specify "%%SANDBOX%%" (with quote marks).
// Are you sure you want to accept this delete command?
//
#define MSG_3856                         0x00000F10L

//
// MessageId: MSG_3857
//
// MessageText:
//
// Please navigate to and select the program - %2
//
#define MSG_3857                         0x00000F11L

//
// MessageId: MSG_3971
//
// MessageText:
//
// Program Groups
//
#define MSG_3971                         0x00000F83L

//
// MessageId: MSG_3972
//
// MessageText:
//
// You can group a number of programs together and give them a group name.  Program groups can be used with some of the settings that accept program names.
//
#define MSG_3972                         0x00000F84L

//
// MessageId: MSG_3974
//
// MessageText:
//
// Add &Group
//
#define MSG_3974                         0x00000F86L

//
// MessageId: MSG_3975
//
// MessageText:
//
// Enter new group name
//
#define MSG_3975                         0x00000F87L

//
// MessageId: MSG_3861
//
// MessageText:
//
// Program Start
//
#define MSG_3861                         0x00000F15L

//
// MessageId: MSG_3862
//
// MessageText:
//
// Forced Folders
//
#define MSG_3862                         0x00000F16L

//
// MessageId: MSG_3863
//
// MessageText:
//
// If any program starts unsandboxed from one of the following folders, it will be forced to run in this sandbox.  This does not apply if the program is explicitly started in another sandbox.
//
#define MSG_3863                         0x00000F17L

//
// MessageId: MSG_3864
//
// MessageText:
//
// You can force Windows CD/DVD AutoRun to run sandboxed, by including your CD-ROM and DVD-ROM drive letters in the following list.
//
#define MSG_3864                         0x00000F18L

//
// MessageId: MSG_3865
//
// MessageText:
//
// Forced Folders take precedence over Forced Programs.
//
#define MSG_3865                         0x00000F19L

//
// MessageId: MSG_3866
//
// MessageText:
//
// Select a folder to add to the list of forced folders.
//
#define MSG_3866                         0x00000F1AL

//
// MessageId: MSG_3872
//
// MessageText:
//
// Forced Programs
//
#define MSG_3872                         0x00000F20L

//
// MessageId: MSG_3873
//
// MessageText:
//
// If any of the following programs starts unsandboxed, it will be forced to run in this sandbox.  This does not apply if the program is explicitly started in another sandbox.
//
#define MSG_3873                         0x00000F21L

//
// MessageId: MSG_3881
//
// MessageText:
//
// Program Stop
//
#define MSG_3881                         0x00000F29L

//
// MessageId: MSG_3882
//
// MessageText:
//
// Lingering Programs
//
#define MSG_3882                         0x00000F2AL

//
// MessageId: MSG_3883
//
// MessageText:
//
// The following programs will be automatically terminated if they are still executing in this sandbox after all other programs have ended.
//
#define MSG_3883                         0x00000F2BL

//
// MessageId: MSG_3892
//
// MessageText:
//
// Leader Programs
//
#define MSG_3892                         0x00000F34L

//
// MessageId: MSG_3893
//
// MessageText:
//
// The following programs are considered primary programs in this sandbox.  Ending these programs will cause all other programs that are still executing in this sandbox to terminate.
//
#define MSG_3893                         0x00000F35L

//
// MessageId: MSG_3901
//
// MessageText:
//
// File Migration
//
#define MSG_3901                         0x00000F3DL

//
// MessageId: MSG_3903
//
// MessageText:
//
// Files have to be migrated (copied) into the sandbox before they can be modified by programs running in that sandbox.  However, for very large files, the migration process takes a long time.
//
#define MSG_3903                         0x00000F3FL

//
// MessageId: MSG_3904
//
// MessageText:
//
// The following number determines the maximum size of a migrate-able file.  Files larger than that size will never be migrated into the sandbox, and will not be modifiable by programs running in the sandbox.
//
#define MSG_3904                         0x00000F40L

//
// MessageId: MSG_3905
//
// MessageText:
//
// Don't migrate files larger than
//
#define MSG_3905                         0x00000F41L

//
// MessageId: MSG_3906
//
// MessageText:
//
// kilobytes
//
#define MSG_3906                         0x00000F42L

//
// MessageId: MSG_3907
//
// MessageText:
//
// When a file is too large to be migrated, message SBIE2102 will be issued, unless the following option is checked.
//
#define MSG_3907                         0x00000F43L

//
// MessageId: MSG_3908
//
// MessageText:
//
// Don't issue a message when a file is too large to migrate
//
#define MSG_3908                         0x00000F44L

//
// MessageId: MSG_3909
//
// MessageText:
//
// The File Migration setting limits the maximum size of a file that can be copied into the sandbox.
// 
// The setting is currently set to %3 kilobytes in sandbox '%2'.
// 
// Click OK to adjust this size limit to %4 kilobytes.
// 
// You can also change this limit by opening the Sandbox Settings window, and expanding the File Migration settings page.
//
#define MSG_3909                         0x00000F45L

//
// MessageId: MSG_3911
//
// MessageText:
//
// Restrictions
//
#define MSG_3911                         0x00000F47L

//
// MessageId: MSG_3912
//
// MessageText:
//
// Internet Access
//
#define MSG_3912                         0x00000F48L

//
// MessageId: MSG_3913
//
// MessageText:
//
// The following programs will be the only programs in this sandbox that can access the Internet.
//
#define MSG_3913                         0x00000F49L

//
// MessageId: MSG_3914
//
// MessageText:
//
// When this feature is enabled, programs that are installed (or downloaded) into this sandbox will never be allowed to access the Internet, even if they match the program name specified above.
//
#define MSG_3914                         0x00000F4AL

//
// MessageId: MSG_3915
//
// MessageText:
//
// Issue message SBIE%2 when access is denied
//
#define MSG_3915                         0x00000F4BL

//
// MessageId: MSG_3916
//
// MessageText:
//
// &Block All Programs
//
#define MSG_3916                         0x00000F4CL

//
// MessageId: MSG_3917
//
// MessageText:
//
// &Allow All Programs
//
#define MSG_3917                         0x00000F4DL

//
// MessageId: MSG_3918
//
// MessageText:
//
// All programs can access the Internet
//
#define MSG_3918                         0x00000F4EL

//
// MessageId: MSG_3919
//
// MessageText:
//
// No program can access the Internet
//
#define MSG_3919                         0x00000F4FL

//
// MessageId: MSG_3920
//
// MessageText:
//
// Internet Access Restrictions are enabled in sandbox '%2', and prevent this program from accessing the Internet.
// 
// Click OK to add program '%3' to Internet Access Restrictions in sandbox '%2'.
// 
// To manage Internet Access Restrictions, open the Sandbox Settings window, and expand the Restrictions settings group.
//
#define MSG_3920                         0x00000F50L

//
// MessageId: MSG_3922
//
// MessageText:
//
// Start/Run Access
//
#define MSG_3922                         0x00000F52L

//
// MessageId: MSG_3923
//
// MessageText:
//
// The following programs will be the only programs in this sandbox that can start and run.
//
#define MSG_3923                         0x00000F53L

//
// MessageId: MSG_3924
//
// MessageText:
//
// When this feature is enabled, programs that are installed (or downloaded) into this sandbox will never be allowed to start or run, even if they match the program name specified above.
//
#define MSG_3924                         0x00000F54L

//
// MessageId: MSG_3928
//
// MessageText:
//
// All programs can start and run
//
#define MSG_3928                         0x00000F58L

//
// MessageId: MSG_3930
//
// MessageText:
//
// Start/Run Access Restrictions are enabled in sandbox '%2', and prevent this program from starting and running.
// 
// Click OK to add program '%3' to Start/Run Access Restrictions in sandbox '%2'.
// 
// To manage Start/Run Access Restrictions, open the Sandbox Settings window, and expand the Restrictions settings group.
//
#define MSG_3930                         0x00000F5AL

//
// MessageId: MSG_3942
//
// MessageText:
//
// Drop Rights
//
#define MSG_3942                         0x00000F66L

//
// MessageId: MSG_3943
//
// MessageText:
//
// Sandboxie has to disable only a few security rights from the programs it supervises in order to guarantee isolation.
//
#define MSG_3943                         0x00000F67L

//
// MessageId: MSG_3944
//
// MessageText:
//
// However, if you are using an administrative or power user account, Sandboxie can disable more security rights, similar to the effect of the DropMyRights utility.
//
#define MSG_3944                         0x00000F68L

//
// MessageId: MSG_3945
//
// MessageText:
//
// Drop rights from Administrators and Power Users groups
//
#define MSG_3945                         0x00000F69L

//
// MessageId: MSG_3949
//
// MessageText:
//
// The Drop Rights setting is enabled in sandbox '%2', and prevents use of Administrator privileges.
// 
// Click OK to disable the Drop Rights setting in sandbox '%2', and permit use of Administrator privileges in the sandbox.
// 
// You can always change the Drop Rights setting by opening the Sandbox Settings window, and expanding the Restrictions settings group.
//
#define MSG_3949                         0x00000F6DL

//
// MessageId: MSG_3955
//
// MessageText:
//
// Network Files
//
#define MSG_3955                         0x00000F73L

//
// MessageId: MSG_3956
//
// MessageText:
//
// Network files and folders are normally visible to sandboxed applications.
//
#define MSG_3956                         0x00000F74L

//
// MessageId: MSG_3957
//
// MessageText:
//
// If you wish to block sandboxed applications from accessing files and folders on your network, you can enable this setting. Individual files and folders can be opened for sandboxed applications by adding them under Resource Access -> File Access.
//
#define MSG_3957                         0x00000F75L

//
// MessageId: MSG_3958
//
// MessageText:
//
// Block network files and folders unless specifically opened.
//
#define MSG_3958                         0x00000F76L

//
// MessageId: MSG_4001
//
// MessageText:
//
// Resource Access::File Access::Direct Access
//
#define MSG_4001                         0x00000FA1L

//
// MessageId: MSG_4002
//
// MessageText:
//
// Direct File Access (OpenFilePath)
//
#define MSG_4002                         0x00000FA2L

//
// MessageId: MSG_4003
//
// MessageText:
//
// The following files and folders will be directly accessible to programs running in this sandbox, without any effects of sandboxing.
//
#define MSG_4003                         0x00000FA3L

//
// MessageId: MSG_4004
//
// MessageText:
//
// This does not apply to programs that have been installed or downloaded into the sandbox.
// See also:  Full Access setting.
//
#define MSG_4004                         0x00000FA4L

//
// MessageId: MSG_4011
//
// MessageText:
//
// Resource Access::File Access::Full Access
//
#define MSG_4011                         0x00000FABL

//
// MessageId: MSG_4012
//
// MessageText:
//
// Full File Access (OpenPipePath)
//
#define MSG_4012                         0x00000FACL

//
// MessageId: MSG_4013
//
// MessageText:
//
// The following files and folders will be directly accessible to programs running in this sandbox, without any effects of sandboxing.
//
#define MSG_4013                         0x00000FADL

//
// MessageId: MSG_4014
//
// MessageText:
//
// Unlike the Direct Access setting, this setting applies to all programs, including those that have been installed or downloaded into the sandbox.
//
#define MSG_4014                         0x00000FAEL

//
// MessageId: MSG_4021
//
// MessageText:
//
// Resource Access::File Access::Blocked Access
//
#define MSG_4021                         0x00000FB5L

//
// MessageId: MSG_4022
//
// MessageText:
//
// Blocked File Access (ClosedFilePath)
//
#define MSG_4022                         0x00000FB6L

//
// MessageId: MSG_4023
//
// MessageText:
//
// The following files and folders will not be accessible at all to programs running in this sandbox.
//
#define MSG_4023                         0x00000FB7L

//
// MessageId: MSG_4024
//
// MessageText:
//
// If a file or folder matches any other File Access setting, but also matches any Blocked Access setting, the Blocked Access setting will take precedence.
//
#define MSG_4024                         0x00000FB8L

//
// MessageId: MSG_4025
//
// MessageText:
//
// Windows file sharing can be used to circumvent Blocked File Access settings.
// Therefore, a setting was added to block access to the Windows file sharing service.
//
#define MSG_4025                         0x00000FB9L

//
// MessageId: MSG_4031
//
// MessageText:
//
// Resource Access::File Access::Read-Only Access
//
#define MSG_4031                         0x00000FBFL

//
// MessageId: MSG_4032
//
// MessageText:
//
// Read-Only File Access (ReadFilePath)
//
#define MSG_4032                         0x00000FC0L

//
// MessageId: MSG_4033
//
// MessageText:
//
// The following files and folders will not be modifiable to programs running in this sandbox.
//
#define MSG_4033                         0x00000FC1L

//
// MessageId: MSG_4111
//
// MessageText:
//
// Resource Access::File Access::Write-Only Access
//
#define MSG_4111                         0x0000100FL

//
// MessageId: MSG_4112
//
// MessageText:
//
// Write-Only File Access (WriteFilePath)
//
#define MSG_4112                         0x00001010L

//
// MessageId: MSG_4113
//
// MessageText:
//
// The following folders will appear empty to programs running in this sandbox, but the programs may create new files within these folders in the sandbox.
//
#define MSG_4113                         0x00001011L

//
// MessageId: MSG_4041
//
// MessageText:
//
// Resource Access::Registry Access::Direct Access
//
#define MSG_4041                         0x00000FC9L

//
// MessageId: MSG_4042
//
// MessageText:
//
// Direct Registry Access (OpenKeyPath)
//
#define MSG_4042                         0x00000FCAL

//
// MessageId: MSG_4043
//
// MessageText:
//
// The following registry keys will be directly accessible to programs running in this sandbox, without any effects of sandboxing.
//
#define MSG_4043                         0x00000FCBL

//
// MessageId: MSG_4044
//
// MessageText:
//
// This does not apply to programs that have been installed or downloaded into the sandbox.
//
#define MSG_4044                         0x00000FCCL

//
// MessageId: MSG_4051
//
// MessageText:
//
// Resource Access::Registry Access::Blocked Access
//
#define MSG_4051                         0x00000FD3L

//
// MessageId: MSG_4052
//
// MessageText:
//
// Blocked Registry Access (ClosedKeyPath)
//
#define MSG_4052                         0x00000FD4L

//
// MessageId: MSG_4053
//
// MessageText:
//
// The following registry keys will not be accessible at all to programs running in this sandbox.
//
#define MSG_4053                         0x00000FD5L

//
// MessageId: MSG_4054
//
// MessageText:
//
// If a registry key matches any other Registry Access setting, but also matches any Blocked Access setting, the Blocked Access setting will take precedence.
//
#define MSG_4054                         0x00000FD6L

//
// MessageId: MSG_4061
//
// MessageText:
//
// Resource Access::Registry Access::Read-Only Access
//
#define MSG_4061                         0x00000FDDL

//
// MessageId: MSG_4062
//
// MessageText:
//
// Read-Only Registry Access (ReadKeyPath)
//
#define MSG_4062                         0x00000FDEL

//
// MessageId: MSG_4063
//
// MessageText:
//
// The following registry keys will not be modifiable to programs running in this sandbox.
//
#define MSG_4063                         0x00000FDFL

//
// MessageId: MSG_4121
//
// MessageText:
//
// Resource Access::Registry Access::Write-Only Access
//
#define MSG_4121                         0x00001019L

//
// MessageId: MSG_4122
//
// MessageText:
//
// Write-Only Registry Access (WriteKeyPath)
//
#define MSG_4122                         0x0000101AL

//
// MessageId: MSG_4123
//
// MessageText:
//
// The following registry keys will appear empty to programs running in this sandbox, but the programs may write new data within these registry keys in the sandbox.
//
#define MSG_4123                         0x0000101BL

//
// MessageId: MSG_4071
//
// MessageText:
//
// Resource Access::IPC Access::Direct Access
//
#define MSG_4071                         0x00000FE7L

//
// MessageId: MSG_4072
//
// MessageText:
//
// Direct IPC Access (OpenIpcPath)
//
#define MSG_4072                         0x00000FE8L

//
// MessageId: MSG_4073
//
// MessageText:
//
// The following NT IPC objects will be directly accessible to programs running in this sandbox, without any effects of sandboxing.
//
#define MSG_4073                         0x00000FE9L

//
// MessageId: MSG_4074
//
// MessageText:
//
// This setting applies to all programs, including those that have been installed or downloaded into the sandbox.
//
#define MSG_4074                         0x00000FEAL

//
// MessageId: MSG_4081
//
// MessageText:
//
// Resource Access::IPC Access::Blocked Access
//
#define MSG_4081                         0x00000FF1L

//
// MessageId: MSG_4082
//
// MessageText:
//
// Blocked IPC Access (ClosedIpcPath)
//
#define MSG_4082                         0x00000FF2L

//
// MessageId: MSG_4083
//
// MessageText:
//
// The following NT IPC objects will not be accessible at all to programs running in this sandbox.
//
#define MSG_4083                         0x00000FF3L

//
// MessageId: MSG_4084
//
// MessageText:
//
// If an NT IPC object matches any other IPC Access setting, but also matches any Blocked Access setting, the Blocked Access setting will take precedence.
//
#define MSG_4084                         0x00000FF4L

//
// MessageId: MSG_4091
//
// MessageText:
//
// Resource Access::Window Access
//
#define MSG_4091                         0x00000FFBL

//
// MessageId: MSG_4092
//
// MessageText:
//
// Window Access (OpenWinClass)
//
#define MSG_4092                         0x00000FFCL

//
// MessageId: MSG_4093
//
// MessageText:
//
// Sandboxed programs cannot communicate with windows that belong to programs running outside the sandbox, unless the window was created with one of the following window classes.
//
#define MSG_4093                         0x00000FFDL

//
// MessageId: MSG_4101
//
// MessageText:
//
// Resource Access::COM Access
//
#define MSG_4101                         0x00001005L

//
// MessageId: MSG_4102
//
// MessageText:
//
// COM Access (OpenClsid)
//
#define MSG_4102                         0x00001006L

//
// MessageId: MSG_4103
//
// MessageText:
//
// Sandboxed programs cannot communicate with COM objects that belong to programs running outside the sandbox, unless the COM class ID matches one of the following class IDs.
//
#define MSG_4103                         0x00001007L

//
// MessageId: MSG_4104
//
// MessageText:
//
// Resource Access::Network Access
//
#define MSG_4104                         0x00001008L

//
// MessageId: MSG_4105
//
// MessageText:
//
// Network Access (Firewall)
//
#define MSG_4105                         0x00001009L

//
// MessageId: MSG_4201
//
// MessageText:
//
// Applications
//
#define MSG_4201                         0x00001069L

//
// MessageId: MSG_4202
//
// MessageText:
//
// &View Code
//
#define MSG_4202                         0x0000106AL

//
// MessageId: MSG_4203
//
// MessageText:
//
// Open &Web Site
//
#define MSG_4203                         0x0000106BL

//
// MessageId: MSG_4204
//
// MessageText:
//
// Create &New
//
#define MSG_4204                         0x0000106CL

//
// MessageId: MSG_4205
//
// MessageText:
//
// This application configuration does not include a Web address.
//
#define MSG_4205                         0x0000106DL

//
// MessageId: MSG_4206
//
// MessageText:
//
// Miscellaneous
//
#define MSG_4206                         0x0000106EL

//
// MessageId: MSG_4207
//
// MessageText:
//
// Improve the use of Sandboxie with your other applications by selecting them from the list below.
//
#define MSG_4207                         0x0000106FL

//
// MessageId: MSG_4209
//
// MessageText:
//
// Folders
//
#define MSG_4209                         0x00001071L

//
// MessageId: MSG_4210
//
// MessageText:
//
// Configure the folder locations used by your other applications.
//
#define MSG_4210                         0x00001072L

//
// MessageId: MSG_4211
//
// MessageText:
//
// Select folder for
//
#define MSG_4211                         0x00001073L

//
// MessageId: MSG_4212
//
// MessageText:
//
// Default location:
//
#define MSG_4212                         0x00001074L

//
// MessageId: MSG_4213
//
// MessageText:
//
// Alternate location:
//
#define MSG_4213                         0x00001075L

//
// MessageId: MSG_4214
//
// MessageText:
//
// An alternate location for '%2'
// should contain the following file:
// 
// %3
// 
// The selected location does not contain this file.
// 
// Please select a folder which contains this file.
//
#define MSG_4214                         0x00001076L

//
// MessageId: MSG_4215
//
// MessageText:
//
// Select the folder used by %2.
//
#define MSG_4215                         0x00001077L

//
// MessageId: MSG_4216
//
// MessageText:
//
// The settings below compose the application configuration '%2'.
//
#define MSG_4216                         0x00001078L

//
// MessageId: MSG_4217
//
// MessageText:
//
// Type or paste settings, then click OK to create a new local application configuration.
//
#define MSG_4217                         0x00001079L

//
// MessageId: MSG_4218
//
// MessageText:
//
// Local
//
#define MSG_4218                         0x0000107AL

//
// MessageId: MSG_4219
//
// MessageText:
//
// Your local (unofficial) application configurations.
//
#define MSG_4219                         0x0000107BL

//
// MessageId: MSG_4220
//
// MessageText:
//
// The entered code contains one or more errors.
// Cannot create the application configuration.
//
#define MSG_4220                         0x0000107CL

//
// MessageId: MSG_4221
//
// MessageText:
//
// To specify folder locations for your applications, see the Folders page.
//
#define MSG_4221                         0x0000107DL

//
// MessageId: MSG_4222
//
// MessageText:
//
// This setting cannot be edited or removed because it is a
// part of an application configuration:
// 
// %2
// 
// To remove the application configuration from the sandbox,
// see the Applications pages in the Sandbox Settings window.
//
#define MSG_4222                         0x0000107EL

//
// MessageId: MSG_4223
//
// MessageText:
//
// Local application configuration '%2' is now removed in all sandboxes.
// Would you like to completely delete this application configuration?
//
#define MSG_4223                         0x0000107FL

//
// MessageId: MSG_4224
//
// MessageText:
//
// PDF/Printing
//
#define MSG_4224                         0x00001080L

//
// MessageId: MSG_4226
//
// MessageText:
//
// Security/Privacy
//
#define MSG_4226                         0x00001082L

//
// MessageId: MSG_4228
//
// MessageText:
//
// Desktop Utilities
//
#define MSG_4228                         0x00001084L

//
// MessageId: MSG_4230
//
// MessageText:
//
// Download Managers
//
#define MSG_4230                         0x00001086L

//
// MessageId: MSG_4288
//
// MessageText:
//
// All Applications
//
#define MSG_4288                         0x000010C0L

//
// MessageId: MSG_4289
//
// MessageText:
//
// List of all application configurations, sorted in alphabetical order.  The list does not include your local (unofficial) application configurations.
//
#define MSG_4289                         0x000010C1L

//
// MessageId: MSG_4291
//
// MessageText:
//
// Default exclusions for Immediate Recovery
//
#define MSG_4291                         0x000010C3L

//
// MessageId: MSG_4292
//
// MessageText:
//
// Default list of Lingering Programs
//
#define MSG_4292                         0x000010C4L

//
// MessageId: MSG_4293
//
// MessageText:
//
// Default list of blocked TCP/IP ports
//
#define MSG_4293                         0x000010C5L

//
// MessageId: MSG_4294
//
// MessageText:
//
// Permit programs to update jump lists in the Windows taskbar
//
#define MSG_4294                         0x000010C6L

//
// MessageId: MSG_4295
//
// MessageText:
//
// Default exclusions File Migration presets
//
#define MSG_4295                         0x000010C7L

//
// MessageId: MSG_4296
//
// MessageText:
//
// Default RPC Port Bindings
//
#define MSG_4296                         0x000010C8L

//
// MessageId: MSG_4297
//
// MessageText:
//
// Open Bluetooth RPC port
//
#define MSG_4297                         0x000010C9L

//
// MessageId: MSG_4298
//
// MessageText:
//
// Open Smart Card RPC port
//
#define MSG_4298                         0x000010CAL

//
// MessageId: MSG_4299
//
// MessageText:
//
// Open Simple Service Discovery Protocol (SSDP, UPnP) RPC port
//
#define MSG_4299                         0x000010CBL

//
// MessageId: MSG_4300
//
// MessageText:
//
// Open additional RPC Port Bindings
//
#define MSG_4300                         0x000010CCL

//
// MessageId: MSG_4306
//
// MessageText:
//
// Open RPC Port Bindings for UAC
//
#define MSG_4306                         0x000010D2L

//
// MessageId: MSG_4307
//
// MessageText:
//
// Block common telemetry processes
//
#define MSG_4307                         0x000010D3L

//
// MessageId: MSG_4308
//
// MessageText:
//
// Filter access to \Devices\
//
#define MSG_4308                         0x000010D4L

//
// MessageId: MSG_4251
//
// MessageText:
//
// Software Compatibility
//
#define MSG_4251                         0x0000109BL

//
// MessageId: MSG_4252
//
// MessageText:
//
// Sandboxie has detected the following software applications in your system.
//
#define MSG_4252                         0x0000109CL

//
// MessageId: MSG_4253
//
// MessageText:
//
// Click OK to apply configuration settings which will improve compatibility with these applications.
//
#define MSG_4253                         0x0000109DL

//
// MessageId: MSG_4254
//
// MessageText:
//
// These configuration settings will have effect in all existing sandboxes and in any new sandboxes.
//
#define MSG_4254                         0x0000109EL

//
// MessageId: MSG_4255
//
// MessageText:
//
// In the future, don't check software compatibility
//
#define MSG_4255                         0x0000109FL

//
// MessageId: MSG_4256
//
// MessageText:
//
// This application configuration cannot be removed because it is applied to all sandboxes.
// To remove this application configuration, use the Software Compatibility tool from the Configure menu.
//
#define MSG_4256                         0x000010A0L

//
// MessageId: MSG_4257
//
// MessageText:
//
// Remove &Old Settings
//
#define MSG_4257                         0x000010A1L

//
// MessageId: MSG_4258
//
// MessageText:
//
// The following compatibility settings are no longer in use:
// 
// %2
// 
// Click OK to discard these compatibility settings.
//
#define MSG_4258                         0x000010A2L

//
// MessageId: MSG_4451
//
// MessageText:
//
// Sandboxie has detected the following conflicting software
// applications in your system.  These applications might
// interfere with the correct operation of Sandboxie.
// 
// %2
// 
// Please review the Known Conflicts page on the Sandboxie
// web site for more information and solutions.
// 
// Would you like to open the Known Conflicts web page now?
//
#define MSG_4451                         0x00001163L

//
// MessageId: MSG_4452
//
// MessageText:
//
// &Known Conflicts
//
#define MSG_4452                         0x00001164L

//
// MessageId: MSG_4261
//
// MessageText:
//
// Lock Configuration
//
#define MSG_4261                         0x000010A5L

//
// MessageId: MSG_4262
//
// MessageText:
//
// Sandboxie can protect global configuration and Sandbox Settings from unauthorized changes.  Select protection modes to enable or disable.
//
#define MSG_4262                         0x000010A6L

//
// MessageId: MSG_4263
//
// MessageText:
//
// Only &Administrator user accounts can make changes
//
#define MSG_4263                         0x000010A7L

//
// MessageId: MSG_4264
//
// MessageText:
//
// &Password must be entered in order to make changes
//
#define MSG_4264                         0x000010A8L

//
// MessageId: MSG_4265
//
// MessageText:
//
// &Forget password when Sandboxie Control window becomes hidden
//
#define MSG_4265                         0x000010A9L

//
// MessageId: MSG_4266
//
// MessageText:
//
// &Change Password
//
#define MSG_4266                         0x000010AAL

//
// MessageId: MSG_4267
//
// MessageText:
//
// Only Administrator user accounts can use &Disable Forced Programs command
//
#define MSG_4267                         0x000010ABL

//
// MessageId: MSG_4269
//
// MessageText:
//
// Please consider reading the Sandboxie documentation page concerning
// protection of the configuration.  Would you like to open this page now?
//
#define MSG_4269                         0x000010ADL

//
// MessageId: MSG_4271
//
// MessageText:
//
// Sandboxie configuration is protected by a password.  Please enter the password now.
//
#define MSG_4271                         0x000010AFL

//
// MessageId: MSG_4272
//
// MessageText:
//
// Enter a new password with which to protect Sandboxie configuration.
//
#define MSG_4272                         0x000010B0L

//
// MessageId: MSG_4273
//
// MessageText:
//
// Enter the same password again to confirm it.
//
#define MSG_4273                         0x000010B1L

//
// MessageId: MSG_4274
//
// MessageText:
//
// Wrong password entered.  Try again?
//
#define MSG_4274                         0x000010B2L

//
// MessageId: MSG_4281
//
// MessageText:
//
// Select a program name
//
#define MSG_4281                         0x000010B9L

//
// MessageId: MSG_4282
//
// MessageText:
//
// Select a program name or a program group
//
#define MSG_4282                         0x000010BAL

//
// MessageId: MSG_4283
//
// MessageText:
//
// Programs that were recently started
//
#define MSG_4283                         0x000010BBL

//
// MessageId: MSG_4284
//
// MessageText:
//
// Programs that appear in Sandbox Settings
//
#define MSG_4284                         0x000010BCL

//
// MessageId: MSG_4285
//
// MessageText:
//
// Program groups
//
#define MSG_4285                         0x000010BDL

//
// MessageId: MSG_4286
//
// MessageText:
//
// Select or &enter a program:
//
#define MSG_4286                         0x000010BEL

//
// MessageId: MSG_4287
//
// MessageText:
//
// Open / Select &File
//
#define MSG_4287                         0x000010BFL

//
// MessageId: MSG_4302
//
// MessageText:
//
// Accessibility
//
#define MSG_4302                         0x000010CEL

//
// MessageId: MSG_4303
//
// MessageText:
//
// The following settings enable the use of Sandboxie in combination with accessibility software.  Please note that some measure of Sandboxie protection is necessarily lost when these settings are in effect.
//
#define MSG_4303                         0x000010CFL

//
// MessageId: MSG_4304
//
// MessageText:
//
// To compensate for the lost protection, please consult the Drop Rights settings page in the Restrictions settings group.
//
#define MSG_4304                         0x000010D0L

//
// MessageId: MSG_4305
//
// MessageText:
//
// Screen Readers:  %2
//
#define MSG_4305                         0x000010D1L

//
// MessageId: MSG_4321
//
// MessageText:
//
// Web Browser
//
#define MSG_4321                         0x000010E1L

//
// MessageId: MSG_4322
//
// MessageText:
//
// The following exclusions allow Web browsers running in this sandbox to make changes outside the sandbox, thus trading a small measure of security and privacy for greater convenience.
//
#define MSG_4322                         0x000010E2L

//
// MessageId: MSG_4323
//
// MessageText:
//
// Force %2 to run in this sandbox
//
#define MSG_4323                         0x000010E3L

//
// MessageId: MSG_4324
//
// MessageText:
//
// Allow direct access to %2 sync data
//
#define MSG_4324                         0x000010E4L

//
// MessageId: MSG_4325
//
// MessageText:
//
// Allow direct access to %2 feeds
//
#define MSG_4325                         0x000010E5L

//
// MessageId: MSG_4326
//
// MessageText:
//
// Allow direct access to %2 favourites
//
#define MSG_4326                         0x000010E6L

//
// MessageId: MSG_4327
//
// MessageText:
//
// Add Internet Explorer favourites to Quick Recovery folders
//
#define MSG_4327                         0x000010E7L

//
// MessageId: MSG_4328
//
// MessageText:
//
// Allow direct access to %2 cookies
//
#define MSG_4328                         0x000010E8L

//
// MessageId: MSG_4329
//
// MessageText:
//
// Save outside sandbox:  History of search strings and invoked commands
//
#define MSG_4329                         0x000010E9L

//
// MessageId: MSG_4330
//
// MessageText:
//
// Save outside sandbox:  Account information for Hotmail and Messenger
//
#define MSG_4330                         0x000010EAL

//
// MessageId: MSG_4331
//
// MessageText:
//
// Allow direct access to %2 passwords
//
#define MSG_4331                         0x000010EBL

//
// MessageId: MSG_4336
//
// MessageText:
//
// Allow direct access to %2 bookmark and history database
//
#define MSG_4336                         0x000010F0L

//
// MessageId: MSG_4337
//
// MessageText:
//
// Allow direct access to %2 phishing database
//
#define MSG_4337                         0x000010F1L

//
// MessageId: MSG_4338
//
// MessageText:
//
// Allow direct access to entire %2 profile folder
//
#define MSG_4338                         0x000010F2L

//
// MessageId: MSG_4339
//
// MessageText:
//
// Allow direct access to %2 preferences
//
#define MSG_4339                         0x000010F3L

//
// MessageId: MSG_4340
//
// MessageText:
//
// Allow direct access to %2 session management
//
#define MSG_4340                         0x000010F4L

//
// MessageId: MSG_4341
//
// MessageText:
//
// Allow direct access to %2 notes
//
#define MSG_4341                         0x000010F5L

//
// MessageId: MSG_4342
//
// MessageText:
//
// Enable %2 compatibility workaround
//
#define MSG_4342                         0x000010F6L

//
// MessageId: MSG_4356
//
// MessageText:
//
// Allow direct access to %2 bookmarks
//
#define MSG_4356                         0x00001104L

//
// MessageId: MSG_4357
//
// MessageText:
//
// Other
//
#define MSG_4357                         0x00001105L

//
// MessageId: MSG_4358
//
// MessageText:
//
// Add-ons
//
#define MSG_4358                         0x00001106L

//
// MessageId: MSG_4391
//
// MessageText:
//
// Email Reader
//
#define MSG_4391                         0x00001127L

//
// MessageId: MSG_4392
//
// MessageText:
//
// The following exclusions allow Email readers running in this sandbox to access mailbox files outside the sandbox.
//
#define MSG_4392                         0x00001128L

//
// MessageId: MSG_4393
//
// MessageText:
//
// Media Players
//
#define MSG_4393                         0x00001129L

//
// MessageId: MSG_4394
//
// MessageText:
//
// The following exclusions allow Media players running in this sandbox to access files outside the sandbox.
//
#define MSG_4394                         0x0000112AL

//
// MessageId: MSG_4395
//
// MessageText:
//
// Allow %2 direct access to the Photo folder for easier screen capture.
//
#define MSG_4395                         0x0000112BL

//
// MessageId: MSG_4396
//
// MessageText:
//
// Torrent Clients
//
#define MSG_4396                         0x0000112CL

//
// MessageId: MSG_4397
//
// MessageText:
//
// The following exclusions allow Torrent clients running in this sandbox to access files outside the sandbox.
//
#define MSG_4397                         0x0000112DL

//
// MessageId: MSG_4398
//
// MessageText:
//
// Allow %2 direct access to the Music folder for easier music library management.
//
#define MSG_4398                         0x0000112EL

//
// MessageId: MSG_5101
//
// MessageText:
//
// User Accounts
//
#define MSG_5101                         0x000013EDL

//
// MessageId: MSG_5102
//
// MessageText:
//
// Add user accounts and user groups to the list below to limit use of the sandbox to only those accounts.  If the list is empty, the sandbox can be used by all user accounts.
//
#define MSG_5102                         0x000013EEL

//
// MessageId: MSG_5103
//
// MessageText:
//
// Note:  Forced Programs and Force Folders settings for a sandbox do not apply to user accounts which cannot use the sandbox.
//
#define MSG_5103                         0x000013EFL

//
// MessageId: MSG_5107
//
// MessageText:
//
// Your user account has now been restricted from using this sandbox, and
// the sandbox will be removed from display in the main window.
// 
// To make the sandbox visible and usable, use the "Reveal Hidden Sandbox"
// command from the Sandbox menu.
//
#define MSG_5107                         0x000013F3L

//
// MessageId: MSG_5122
//
// MessageText:
//
// Your user account is excluded from using any of the sandboxes listed below.
//
#define MSG_5122                         0x00001402L

//
// MessageId: MSG_5123
//
// MessageText:
//
// The following user accounts are permitted to use the selected sandbox at this time.
//
#define MSG_5123                         0x00001403L

//
// MessageId: MSG_5124
//
// MessageText:
//
// Click the OK button to add your user account '%2' to the sandbox and have the sandbox appear in the main window.
//
#define MSG_5124                         0x00001404L

//
// MessageId: MSG_5142
//
// MessageText:
//
// Arrange sandboxes into groups and customize the order in which they are listed.  Use the right-click context menu to manage entries in the layout tree below.
//
#define MSG_5142                         0x00001416L

//
// MessageId: MSG_5143
//
// MessageText:
//
// (Top)
//
#define MSG_5143                         0x00001417L

//
// MessageId: MSG_5145
//
// MessageText:
//
// &Restore Default Layout
//
#define MSG_5145                         0x00001419L

//
// MessageId: MSG_5146
//
// MessageText:
//
// Insert &Group
//
#define MSG_5146                         0x0000141AL

//
// MessageId: MSG_5147
//
// MessageText:
//
// &Rename
//
#define MSG_5147                         0x0000141BL

//
// MessageId: MSG_5148
//
// MessageText:
//
// &Delete
//
#define MSG_5148                         0x0000141CL

//
// MessageId: MSG_5151
//
// MessageText:
//
// &Move
//
#define MSG_5151                         0x0000141FL

//
// MessageId: MSG_5152
//
// MessageText:
//
// Move &Up
//
#define MSG_5152                         0x00001420L

//
// MessageId: MSG_5153
//
// MessageText:
//
// Move &Down
//
#define MSG_5153                         0x00001421L

//
// MessageId: MSG_5154
//
// MessageText:
//
// Move &To
//
#define MSG_5154                         0x00001422L

//
// MessageId: MSG_6001
//
// MessageText:
//
// The classic Sandboxie UI (SbieCtrl.exe) has very limited
// resource monitoring and event tracing capabilities.
// 
// For optimal troubleshooting it is highly advisable 
// to install the new UI, Sandboxie-Plus (SandMan.exe),
// which provides excellent tracing and monitoring capabilities.
// 
// Do you want to download Sandboxie-Plus now?
//
#define MSG_6001                         0x00001771L

//
// MessageId: MSG_6002
//
// MessageText:
//
// Visit <a ID="whats_new">sandboxie-plus.com</a> to learn about the new functionality of Sandboxie-Plus,
// or directly click <a ID="upgrade">here</a> to download the latest Sandboxie-Plus installer.
//
#define MSG_6002                         0x00001772L

//
// MessageId: MSG_6003
//
// MessageText:
//
// The legacy UI of Sandboxie classic does not implement support for the network firewall functionality.
// 
// You can though configure the feature supported by the core components using the ini file, rules are structured like following:
// NetworkAccess=iexplorer.exe,Allow; Port=80,443; Address=192.168.0.1-192.168.100.255; Protocol=TCP
// 
// If you prefer to use a UI to control these options, please upgrade to Sandboxie-Plus, which has full UI support for all new features.
//
#define MSG_6003                         0x00001773L

//
// MessageId: MSG_6005
//
// MessageText:
//
// Note: Without administrative privileges installers will fail to start in the sandbox. Sandboxie-Plus offers an option to make many installers succeed without those privileges. You can enable this option also in the classic build manually, by adding "FakeAdminRights=y" to the ini section for this box.
//
#define MSG_6005                         0x00001775L

//
// MessageId: MSG_6006
//
// MessageText:
//
// The legacy UI of Sandboxie classic implements only support for a couple of restriction options.
// 
// Additional options like "ClosePrintSpooler=y", "OpenClipboard=n", "BlockNetParam=n" and much more can be set in the ini section for this box.
// The modern UI (SandMan.exe) also offers more customization options for Start/Run and Internet access restrictions.
// 
// If you would prefer to use a UI to control these options, please upgrade to Sandboxie-Plus, which has full UI support for all new features.
//
#define MSG_6006                         0x00001776L

//
// MessageId: MSG_6007
//
// MessageText:
//
// Sandboxie Plus offers enhanced privacy protection by switching the old behavior from a black-list mode, i.e., allowing read access to the entire drive, except blocked by Closed Path or Write Path, to a white-list mode where sandboxed programs are only allowed to read generic system locations and read access to most user data must be first explicitly granted.
// 
// The legacy UI of Sandboxie classic does not implement support for this mode of operation. Although it can be configured using the ini file, it is recommended to use the modern Sandboxie-Plus UI (SandMan.exe) if you want to use privacy enhanced boxes.
//
#define MSG_6007                         0x00001777L

//
// MessageId: MSG_7850
//
// MessageText:
//
// Getting Started Tutorial - Sandboxie
//
#define MSG_7850                         0x00001EAAL

//
// MessageId: MSG_7851
//
// MessageText:
//
// Getting Started with Sandboxie
//
#define MSG_7851                         0x00001EABL

//
// MessageId: MSG_7852
//
// MessageText:
//
// Welcome to Sandboxie!
// 
// Sandboxie runs your programs in an isolated space which prevents them from making permanent changes to other programs and data in your computer.
// 
// This tutorial briefly explains the basic principles of using Sandboxie.
//
#define MSG_7852                         0x00001EACL

//
// MessageId: MSG_7853
//
// MessageText:
//
// Note:  This tutorial is designed for use with the default sandbox, DefaultBox.
//
#define MSG_7853                         0x00001EADL

//
// MessageId: MSG_7854
//
// MessageText:
//
// How Sandboxie Works (Illustration)
//
#define MSG_7854                         0x00001EAEL

//
// MessageId: MSG_7855
//
// MessageText:
//
// Start your Internet Web browser under Sandboxie
//
#define MSG_7855                         0x00001EAFL

//
// MessageId: MSG_7856
//
// MessageText:
//
// Please find the shortcut on your desktop and double-click it to run your web browser in the default sandbox, DefaultBox.
//
#define MSG_7856                         0x00001EB0L

//
// MessageId: MSG_7857
//
// MessageText:
//
// Click here to hide this window and show the desktop
//
#define MSG_7857                         0x00001EB1L

//
// MessageId: MSG_7858
//
// MessageText:
//
// Your browser is now running inside the sandbox
//
#define MSG_7858                         0x00001EB2L

//
// MessageId: MSG_7859
//
// MessageText:
//
// Programs in the sandbox can modify only the contents of the sandbox.
// 
// When you save or download files, they go into the sandbox.  If you save a file to your Desktop, Documents or Downloads folders, Sandboxie offers to move the file out of the sandbox.
// 
// Try it now:  Download a file in your browser to your desktop folder (in the sandbox), and recover the file to your real desktop.
//
#define MSG_7859                         0x00001EB3L

//
// MessageId: MSG_7860
//
// MessageText:
//
// Delete the contents of your sandbox
//
#define MSG_7860                         0x00001EB4L

//
// MessageId: MSG_7861
//
// MessageText:
//
// You can think of the sandbox as a piece of transparent paper placed between the program and your computer.  Deleting the contents of the sandbox is like discarding a used piece of paper and replacing it with a clean one.
//
#define MSG_7861                         0x00001EB5L

//
// MessageId: MSG_7862
//
// MessageText:
//
// Try it now:  Delete the contents of your DefaultBox sandbox.  Please find the yellow Sandboxie icon in the highlighted corner of your screen, and click:
//
#define MSG_7862                         0x00001EB6L

//
// MessageId: MSG_7863
//
// MessageText:
//
// Right-click on the yellow Sandboxie icon,
// 
// then select DefaultBox, and invoke Delete Contents.
//
#define MSG_7863                         0x00001EB7L

//
// MessageId: MSG_7864
//
// MessageText:
//
// Ready to go!
//
#define MSG_7864                         0x00001EB8L

//
// MessageId: MSG_7865
//
// MessageText:
//
// Thank you for taking the time to go through this tutorial.  You can visit the Sandboxie web site for more tutorials and tips.
// 
// The default settings in Sandboxie provide full protection, but you may wish to review the sandbox configuration commands in the Sandbox menu in the Sandboxie Control program window.
//
#define MSG_7865                         0x00001EB9L

//
// MessageId: MSG_8101
//
// MessageText:
//
// Cannot install the driver at this time.
// 
// Please restart your computer,
// then rerun the installation procedure.
// 
// Failing function is: %2
//
#define MSG_8101                         0x00001FA5L

//
// MessageId: MSG_8102
//
// MessageText:
//
// Cannot stop the service at this time.
// The service '%2' is busy.
// 
// Please click OK to retry.
//
#define MSG_8102                         0x00001FA6L

//
// MessageId: MSG_8103
//
// MessageText:
//
// This is the first of three retries.
//
#define MSG_8103                         0x00001FA7L

//
// MessageId: MSG_8104
//
// MessageText:
//
// This is the second of three retries.
//
#define MSG_8104                         0x00001FA8L

//
// MessageId: MSG_8105
//
// MessageText:
//
// This is the third and last retry.
//
#define MSG_8105                         0x00001FA9L

//
// MessageId: MSG_8106
//
// MessageText:
//
// The following programs must be closed before the installation can continue.
// Click OK to close these programs and continue.  Click Cancel to abort the installation.
//
#define MSG_8106                         0x00001FAAL

