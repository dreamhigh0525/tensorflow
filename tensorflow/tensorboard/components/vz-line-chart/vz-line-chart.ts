/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
/* tslint:disable:no-namespace variable-name */

module VZ {
  export class LineChart {
    private name2datasets: {[name: string]: Plottable.Dataset};
    private seriesNames: string[];

    private xAccessor: Plottable.Accessor<number|Date>;
    private xScale: Plottable.QuantitativeScale<number|Date>;
    private yScale: Plottable.QuantitativeScale<number>;
    private gridlines: Plottable.Components.Gridlines;
    private center: Plottable.Components.Group;
    private xAxis: Plottable.Axes.Numeric|Plottable.Axes.Time;
    private yAxis: Plottable.Axes.Numeric;
    private outer: Plottable.Components.Table;
    private colorScale: Plottable.Scales.Color;
    private tooltip: d3.Selection<any>;
    private dzl: Plottable.DragZoomLayer;

    private linePlot: Plottable.Plots.Line<number|Date>;
    private smoothLinePlot: Plottable.Plots.Line<number|Date>;
    private scatterPlot: Plottable.Plots.Scatter<number|Date, Number>;
    private nanDisplay: Plottable.Plots.Scatter<number|Date, Number>;
    private yAccessor: Plottable.Accessor<number>;
    private lastPointsDataset: Plottable.Dataset;
    private datasets: Plottable.Dataset[];
    private smoothDatasets: Plottable.Dataset[];
    private name2smoothDatasets: {[name: string]: Plottable.Dataset};
    private onDatasetChanged: (dataset: Plottable.Dataset) => void;
    private nanDataset: Plottable.Dataset;
    private smoothingDecay: number;
    private smoothingEnabled: Boolean;

    constructor(
        xType: string, colorScale: Plottable.Scales.Color,
        tooltip: d3.Selection<any>) {
      this.seriesNames = [];
      this.name2datasets = {};
      this.colorScale = colorScale;
      this.tooltip = tooltip;
      this.datasets = [];
      this.smoothDatasets = [];
      this.name2smoothDatasets = {};
      // lastPointDataset is a dataset that contains just the last point of
      // every dataset we're currently drawing.
      this.lastPointsDataset = new Plottable.Dataset();
      this.nanDataset = new Plottable.Dataset();
      // need to do a single bind, so we can deregister the callback from
      // old Plottable.Datasets. (Deregistration is done by identity checks.)
      this.onDatasetChanged = this._onDatasetChanged.bind(this);
      this.buildChart(xType);
    }

    private buildChart(xType: string) {
      if (this.outer) {
        this.outer.destroy();
      }
      let xComponents = VZ.ChartHelpers.getXComponents(xType);
      this.xAccessor = xComponents.accessor;
      this.xScale = xComponents.scale;
      this.xAxis = xComponents.axis;
      this.xAxis.margin(0).tickLabelPadding(3);
      this.yScale = new Plottable.Scales.Linear();
      this.yAxis = new Plottable.Axes.Numeric(this.yScale, 'left');
      let yFormatter = VZ.ChartHelpers.multiscaleFormatter(
          VZ.ChartHelpers.Y_AXIS_FORMATTER_PRECISION);
      this.yAxis.margin(0).tickLabelPadding(5).formatter(yFormatter);
      this.yAxis.usesTextWidthApproximation(true);

      this.dzl = new Plottable.DragZoomLayer(this.xScale, this.yScale);

      let center = this.buildPlot(this.xAccessor, this.xScale, this.yScale);

      this.gridlines =
          new Plottable.Components.Gridlines(this.xScale, this.yScale);

      this.center =
          new Plottable.Components.Group([this.gridlines, center, this.dzl]);
      this.outer =  new Plottable.Components.Table([
                                                   [this.yAxis, this.center],
                                                   [null, this.xAxis]
                                                  ]);
    }

    private buildPlot(xAccessor, xScale, yScale): Plottable.Component {
      this.yAccessor = (d: VZ.ChartHelpers.ScalarDatum) => d.scalar;
      let linePlot = new Plottable.Plots.Line<number|Date>();
      linePlot.x(xAccessor, xScale);
      linePlot.y(this.yAccessor, yScale);
      linePlot.attr(
          'stroke', (d: VZ.ChartHelpers.Datum, i: number,
                     dataset: Plottable.Dataset) =>
                        this.colorScale.scale(dataset.metadata().name));
      this.linePlot = linePlot;
      let group = this.setupTooltips(linePlot);

      let smoothLinePlot = new Plottable.Plots.Line<number|Date>();
      smoothLinePlot.x(xAccessor, xScale);
      smoothLinePlot.y(this.yAccessor, yScale);
      smoothLinePlot.attr(
          'stroke', (d: VZ.ChartHelpers.Datum, i: number,
                     dataset: Plottable.Dataset) =>
                        this.colorScale.scale(dataset.metadata().name));
      this.smoothLinePlot = smoothLinePlot;

      // The scatterPlot will display the last point for each dataset.
      // This way, if there is only one datum for the series, it is still
      // visible. We hide it when tooltips are active to keep things clean.
      let scatterPlot = new Plottable.Plots.Scatter<number|Date, number>();
      scatterPlot.x(xAccessor, xScale);
      scatterPlot.y(this.yAccessor, yScale);
      scatterPlot.attr('fill', (d: any) => this.colorScale.scale(d.name));
      scatterPlot.attr('opacity', 1);
      scatterPlot.size(VZ.ChartHelpers.TOOLTIP_CIRCLE_SIZE * 2);
      scatterPlot.datasets([this.lastPointsDataset]);
      this.scatterPlot = scatterPlot;

      let nanDisplay = new Plottable.Plots.Scatter<number|Date, number>();
      nanDisplay.x(xAccessor, xScale);
      nanDisplay.y((x) => x.displayY, yScale);
      nanDisplay.attr('fill', (d: any) => this.colorScale.scale(d.name));
      nanDisplay.attr('opacity', 1);
      nanDisplay.size(VZ.ChartHelpers.NAN_SYMBOL_SIZE * 2);
      nanDisplay.datasets([this.nanDataset]);
      nanDisplay.symbol(Plottable.SymbolFactories.triangleUp);
      this.nanDisplay = nanDisplay;

      return new Plottable.Components.Group(
          [nanDisplay, scatterPlot, smoothLinePlot, group]);
    }

    /** Updates the chart when a dataset changes. Called every time the data of
     * a dataset changes to update the charts.
     */
    private _onDatasetChanged(dataset: Plottable.Dataset) {
      if (this.smoothingEnabled) {
        this.resmoothDataset(this.getSmoothDataset(dataset.metadata().name));
        this.updateSpecialDatasets(this.smoothDatasets);
      } else {
        this.updateSpecialDatasets(this.datasets);
      }
    }

    /** Constructs special datasets. Each special dataset contains exceptional
     * values from all of the regular datasets, e.g. last points in series, or
     * NaN values. Those points will have a `name` and `relative` property added
     * (since usually those are context in the surrounding dataset).
     */
    private updateSpecialDatasets(datasets: Plottable.Dataset[]) {
      let lastPointsData =
          datasets
              .map((d) => {
                let datum = null;
                // filter out NaNs to ensure last point is a clean one
                let nonNanData = d.data().filter((x) => !isNaN(x.scalar));
                if (nonNanData.length > 0) {
                  let idx = nonNanData.length - 1;
                  datum = nonNanData[idx];
                  datum.name = d.metadata().name;
                  datum.relative =
                      VZ.ChartHelpers.relativeAccessor(datum, -1, d);
                }
                return datum;
              })
              .filter((x) => x != null);
      this.lastPointsDataset.data(lastPointsData);

      // Take a dataset, return an array of NaN data points
      // the NaN points will have a "displayY" property which is the
      // y-value of a nearby point that was not NaN (0 if all points are NaN)
      let datasetToNaNData = (d: Plottable.Dataset) => {
        let displayY = null;
        let data = d.data();
        let i = 0;
        while (i < data.length && displayY == null) {
          if (!isNaN(data[i].scalar)) {
            displayY = data[i].scalar;
          }
          i++;
        }
        if (displayY == null) {
          displayY = 0;
        }
        let nanData = [];
        for (i = 0; i < data.length; i++) {
          if (!isNaN(data[i].scalar)) {
            displayY = data[i].scalar;
          } else {
            data[i].name = d.metadata().name;
            data[i].displayY = displayY;
            data[i].relative = VZ.ChartHelpers.relativeAccessor(data[i], -1, d);
            nanData.push(data[i]);
          }
        }
        return nanData;
      };
      let nanData = _.flatten(datasets.map(datasetToNaNData));
      this.nanDataset.data(nanData);
    }

    private setupTooltips(plot: Plottable.XYPlot<number|Date, number>):
        Plottable.Components.Group {
      let pi = new Plottable.Interactions.Pointer();
      pi.attachTo(plot);
      // PointsComponent is a Plottable Component that will hold the little
      // circles we draw over the closest data points
      let pointsComponent = new Plottable.Component();
      let group = new Plottable.Components.Group([plot, pointsComponent]);

      let hideTooltips = () => {
        this.tooltip.style('opacity', 0);
        this.scatterPlot.attr('opacity', 1);
        pointsComponent.content().selectAll('.point').remove();
      };

      let enabled = true;
      let disableTooltips = () => {
        enabled = false;
        hideTooltips();
      };
      let enableTooltips = () => { enabled = true; };

      this.dzl.interactionStart(disableTooltips);
      this.dzl.interactionEnd(enableTooltips);

      pi.onPointerMove((p: Plottable.Point) => {
        if (!enabled) {
          return;
        }
        let target: VZ.ChartHelpers.Point = {
          x: p.x,
          y: p.y,
          datum: null,
          dataset: null,
        };

        let centerBBox: SVGRect =
            (<any>this.gridlines.content().node()).getBBox();
        let datasets =
            this.smoothingEnabled ? this.smoothDatasets : plot.datasets();
        let points =
            datasets.map((dataset) => this.findClosestPoint(target, dataset));
        let pointsToCircle = points.filter(
            (p) => p != null &&
                Plottable.Utils.DOM.intersectsBBox(p.x, p.y, centerBBox));
        let pts: any = pointsComponent.content().selectAll('.point').data(
            pointsToCircle,
            (p: VZ.ChartHelpers.Point) => p.dataset.metadata().name);
        if (points.length !== 0) {
          pts.enter().append('circle').classed('point', true);
          pts.attr('r', VZ.ChartHelpers.TOOLTIP_CIRCLE_SIZE)
              .attr('cx', (p) => p.x)
              .attr('cy', (p) => p.y)
              .style('stroke', 'none')
              .attr(
                  'fill',
                  (p) => this.colorScale.scale(p.dataset.metadata().name));
          pts.exit().remove();
          this.drawTooltips(points, target);
        } else {
          hideTooltips();
        }
      });

      pi.onPointerExit(hideTooltips);

      return group;
    }

    private drawTooltips(
        points: VZ.ChartHelpers.Point[], target: VZ.ChartHelpers.Point) {
      // Formatters for value, step, and wall_time
      this.scatterPlot.attr('opacity', 0);
      let valueFormatter = VZ.ChartHelpers.multiscaleFormatter(
          VZ.ChartHelpers.Y_TOOLTIP_FORMATTER_PRECISION);

      let dist = (p: VZ.ChartHelpers.Point) =>
          Math.pow(p.x - target.x, 2) + Math.pow(p.y - target.y, 2);
      let closestDist = _.min(points.map(dist));
      points = _.sortBy(points, (d) => d.dataset.metadata().name);

      let rows = this.tooltip.select('tbody')
                     .html('')
                     .selectAll('tr')
                     .data(points)
                     .enter()
                     .append('tr');
      // Grey out the point if any of the following are true:
      // - The cursor is outside of the x-extent of the dataset
      // - The point is rendered above or below the screen
      // - The point's y value is NaN
      rows.classed('distant', (d) => {
        let firstPoint = d.dataset.data()[0];
        let lastPoint = _.last(d.dataset.data());
        let firstX =
            this.xScale.scale(this.xAccessor(firstPoint, 0, d.dataset));
        let lastX = this.xScale.scale(this.xAccessor(lastPoint, 0, d.dataset));
        let s = d.datum.scalar;
        let yD = this.yScale.domain();
        return target.x < firstX || target.x > lastX || s < yD[0] ||
            s > yD[1] || isNaN(s);
      });
      rows.classed('closest', (p) => dist(p) === closestDist);
      // It is a bit hacky that we are manually applying the width to the swatch
      // and the nowrap property to the text here. The reason is as follows:
      // the style gets updated asynchronously by Polymer scopeSubtree observer.
      // Which means we would get incorrect sizing information since the text
      // would wrap by default. However, we need correct measurements so that
      // we can stop the text from falling off the edge of the screen.
      // therefore, we apply the size-critical styles directly.
      rows.style('white-space', 'nowrap');
      rows.append('td')
          .append('span')
          .classed('swatch', true)
          .style(
              'background-color',
              (d) => this.colorScale.scale(d.dataset.metadata().name));
      rows.append('td').text((d) => d.dataset.metadata().name);
      rows.append('td').text(
          (d) =>
              isNaN(d.datum.scalar) ? 'NaN' : valueFormatter(d.datum.scalar));
      rows.append('td').text(
          (d) => VZ.ChartHelpers.stepFormatter(d.datum.step));
      rows.append('td').text(
          (d) => VZ.ChartHelpers.timeFormatter(d.datum.wall_time));
      rows.append('td').text(
          (d) => VZ.ChartHelpers.relativeFormatter(
              VZ.ChartHelpers.relativeAccessor(d.datum, -1, d.dataset)));

      // compute left position
      let documentWidth = document.body.clientWidth;
      let node: any = this.tooltip.node();
      let parentRect = node.parentElement.getBoundingClientRect();
      let nodeRect = node.getBoundingClientRect();
      // prevent it from falling off the right side of the screen
      let left =
          Math.min(0, documentWidth - parentRect.left - nodeRect.width - 60);
      this.tooltip.style('left', left + 'px');
      // compute top position
      if (parentRect.bottom + nodeRect.height +
              VZ.ChartHelpers.TOOLTIP_Y_PIXEL_OFFSET <
          document.body.clientHeight) {
        this.tooltip.style(
            'top', parentRect.bottom + VZ.ChartHelpers.TOOLTIP_Y_PIXEL_OFFSET);
      } else {
        this.tooltip.style(
            'bottom', parentRect.top - VZ.ChartHelpers.TOOLTIP_Y_PIXEL_OFFSET);
      }

      this.tooltip.style('opacity', 1);
    }

    private findClosestPoint(
        target: VZ.ChartHelpers.Point,
        dataset: Plottable.Dataset): VZ.ChartHelpers.Point {
      let points: VZ.ChartHelpers.Point[] = dataset.data().map((d, i) => {
        let x = this.xAccessor(d, i, dataset);
        let y = this.yAccessor(d, i, dataset);
        return {
          x: this.xScale.scale(x),
          y: this.yScale.scale(y),
          datum: d,
          dataset: dataset,
        };
      });
      let idx: number =
          _.sortedIndex(points, target, (p: VZ.ChartHelpers.Point) => p.x);
      if (idx === points.length) {
        return points[points.length - 1];
      } else if (idx === 0) {
        return points[0];
      } else {
        let prev = points[idx - 1];
        let next = points[idx];
        let prevDist = Math.abs(prev.x - target.x);
        let nextDist = Math.abs(next.x - target.x);
        return prevDist < nextDist ? prev : next;
      }
    }

    private getSmoothDataset(name: string) {
      if (this.name2smoothDatasets[name] === undefined) {
        this.name2smoothDatasets[name] =
            new Plottable.Dataset([], {name: name});
      }
      return this.name2smoothDatasets[name];
    }

    private resmoothDataset(dataset: Plottable.Dataset) {
      let unsmoothedData = this.getDataset(dataset.metadata().name).data();

      // EMA with first step initialized to first element.
      let smoothedData = _.cloneDeep(unsmoothedData);
      smoothedData.forEach((d, i) => {
        if (i === 0) {
          return;
        }
        d.scalar = (1.0 - this.smoothingDecay) * d.scalar +
            this.smoothingDecay * smoothedData[i - 1].scalar;
      });

      dataset.data(smoothedData);
    }

    private getDataset(name: string) {
      if (this.name2datasets[name] === undefined) {
        this.name2datasets[name] = new Plottable.Dataset([], {name: name});
      }
      return this.name2datasets[name];
    }

    /**
     * Update the selected series on the chart.
     */
    public setVisibleSeries(names: string[]) {
      this.seriesNames = names;

      names.reverse();  // draw first series on top
      this.datasets.forEach((d) => d.offUpdate(this.onDatasetChanged));
      this.datasets = names.map((r) => this.getDataset(r));
      this.datasets.forEach((d) => d.onUpdate(this.onDatasetChanged));
      this.linePlot.datasets(this.datasets);

      if (this.smoothingEnabled) {
        this.smoothDatasets = names.map((r) => this.getSmoothDataset(r));
        this.smoothLinePlot.datasets(this.smoothDatasets);
      }
    }

    /**
     * Set the data of a series on the chart.
     */
    public setSeriesData(name: string, data: VZ.ChartHelpers.ScalarDatum[]) {
      this.getDataset(name).data(data);
    }

    public smoothingUpdate(decay: number) {
      if (!this.smoothingEnabled) {
        this.linePlot.addClass('ghost');
        this.smoothingEnabled = true;
        this.smoothDatasets =
            this.seriesNames.map((r) => this.getSmoothDataset(r));
        this.smoothLinePlot.datasets(this.smoothDatasets);
      }

      this.smoothingDecay = decay;
      this.smoothDatasets.forEach((d) => this.resmoothDataset(d));
      this.updateSpecialDatasets(this.smoothDatasets);
    }

    public smoothingDisable() {
      if (this.smoothingEnabled) {
        this.linePlot.removeClass('ghost');
        this.smoothDatasets = [];
        this.smoothLinePlot.datasets(this.smoothDatasets);
        this.smoothingEnabled = false;
        this.updateSpecialDatasets(this.datasets);
      }
    }

    public renderTo(target: d3.Selection<any>) { this.outer.renderTo(target); }

    public redraw() { this.outer.redraw(); }

    public destroy() { this.outer.destroy(); }
  }
}
