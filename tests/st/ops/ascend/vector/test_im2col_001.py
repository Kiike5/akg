# Copyright 2019 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
################################################

Testcase_PrepareCondition:

Testcase_TestSteps:

Testcase_ExpectedResult:

"""
import os
import pytest
from tests.common.base import TestBase
from tests.common.test_run.im2col_run import im2col_run

############################################################
# TestCase= class: put to tests/*/
############################################################


class TestCase(TestBase):

    def setup(self):
        case_name = "test_auto_tensor_im2col_001"
        case_path = os.getcwd()
        self.params_init(case_name, case_path)
        self.caseresult = True
        self._log.info("============= {0} Setup case============".format(self.casename))
        self.testarg = [
            # # testflag,opfuncname,testRunArgs:shape,kernel,stride,pad,dtype,polyhedral,attr
            ("mansch_im2col", im2col_run, ((1, 1, 30, 30, 16), (2, 2), (2, 2), (1, 1, 1, 1), "float16")),
            ("mansch_im2col", im2col_run, ((1, 1, 32, 32, 16), (2, 2), (2, 2), (0, 0, 0, 0), "float16")),
            ("mansch_im2col", im2col_run, ((1, 4, 32, 32, 16), (2, 2), (2, 2), (0, 0, 0, 0), "float16")),
            ("mansch_im2col", im2col_run, ((1, 1, 33, 33, 16), (3, 3), (2, 2), (0, 0, 0, 0), "float16")),
            ("mansch_im2col", im2col_run, ((1, 1, 48, 48, 16), (3, 3), (3, 3), (0, 0, 0, 0), "float16")),
            ("mansch_im2col", im2col_run, ((1, 1, 64, 64, 16), (2, 2), (2, 2), (0, 0, 0, 0), "float16")),
            ("mansch_im2col", im2col_run, ((1, 1, 96, 96, 16), (3, 3), (3, 3), (0, 0, 0, 0), "float16")),
        ]
        return

    @pytest.mark.level0
    @pytest.mark.platform_arm_ascend_training
    @pytest.mark.platform_x86_ascend_training
    @pytest.mark.env_onecard
    def test_run(self):
        self.common_run(self.testarg)

    def teardown(self):
        self._log.info("============= {0} Teardown============".format(self.casename))
        return


if __name__ == "__main__":
    t = TestCase()
    t.setup()
    t.test_run()
